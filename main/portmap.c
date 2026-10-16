#include "portmap.h"

#include <esp_log.h>
#include <nvs_flash.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "proxy.h"

static const char *TAG = "portmap";

#define NVS_NAMESPACE  "portmap"
#define NVS_KEY_JSON   "entries"

// Defaults sembrados en primer boot o tras /portmap defaults.
static const portmap_entry_t DEFAULTS[] = {
    {
        .local_port    = 8080,
        .upstream_host = "",
        .upstream_port = 8080,
        .proto         = "http",
        .label         = "app",
    },
};
#define DEFAULTS_COUNT (sizeof(DEFAULTS) / sizeof(DEFAULTS[0]))

// Estado global. Mutex protege s_entries + s_listeners.
static portmap_entry_t     s_entries[PORTMAP_MAX_ENTRIES];
static int                 s_count          = 0;
static proxy_listener_ctx_t *s_listeners[PORTMAP_MAX_ENTRIES];
static int                 s_listener_count = 0;
static SemaphoreHandle_t   s_mutex          = NULL;

// ─── NVS helpers ──────────────────────────────────────────────
static esp_err_t save_to_nvs(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open write failed: %s", esp_err_to_name(err));
        return err;
    }

    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        nvs_close(h);
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < s_count; i++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "local_port", s_entries[i].local_port);
        cJSON_AddStringToObject(obj, "upstream_host",
                                s_entries[i].upstream_host);
        cJSON_AddNumberToObject(obj, "upstream_port", s_entries[i].upstream_port);
        cJSON_AddStringToObject(obj, "proto", s_entries[i].proto);
        cJSON_AddStringToObject(obj, "label", s_entries[i].label);
        cJSON_AddItemToArray(arr, obj);
    }
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!json) {
        nvs_close(h);
        return ESP_ERR_NO_MEM;
    }

    err = nvs_set_str(h, NVS_KEY_JSON, json);
    free(json);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_str/commit failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "saved %d entries to NVS", s_count);
    }
    return err;
}

static esp_err_t load_from_nvs(int *out_loaded) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open read failed: %s", esp_err_to_name(err));
        return err;
    }

    // Buffer en heap para no inflar el stack de app_main.
    char *json_buf = malloc(2048);
    if (!json_buf) {
        nvs_close(h);
        return ESP_ERR_NO_MEM;
    }
    size_t len = 2048;
    err = nvs_get_str(h, NVS_KEY_JSON, json_buf, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        free(json_buf);
        return err;
    }

    cJSON *arr = cJSON_Parse(json_buf);
    free(json_buf);
    if (!arr || !cJSON_IsArray(arr)) {
        if (arr) cJSON_Delete(arr);
        ESP_LOGW(TAG, "NVS JSON malformed");
        return ESP_FAIL;
    }

    int loaded = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        if (loaded >= PORTMAP_MAX_ENTRIES) break;
        cJSON *lp = cJSON_GetObjectItemCaseSensitive(item, "local_port");
        cJSON *uh = cJSON_GetObjectItemCaseSensitive(item, "upstream_host");
        cJSON *up = cJSON_GetObjectItemCaseSensitive(item, "upstream_port");
        cJSON *pr = cJSON_GetObjectItemCaseSensitive(item, "proto");
        cJSON *lb = cJSON_GetObjectItemCaseSensitive(item, "label");
        if (!cJSON_IsNumber(lp) || !cJSON_IsNumber(up)) continue;

        portmap_entry_t *e = &s_entries[loaded];
        memset(e, 0, sizeof(*e));
        e->local_port    = (uint16_t)lp->valuedouble;
        if (cJSON_IsString(uh) && uh->valuestring) {
            strncpy(e->upstream_host, uh->valuestring,
                    sizeof(e->upstream_host) - 1);
        }
        e->upstream_port = (uint16_t)up->valuedouble;
        if (cJSON_IsString(pr) && pr->valuestring) {
            strncpy(e->proto, pr->valuestring, sizeof(e->proto) - 1);
        } else {
            strncpy(e->proto, "http", sizeof(e->proto) - 1);
        }
        if (cJSON_IsString(lb) && lb->valuestring) {
            strncpy(e->label, lb->valuestring, sizeof(e->label) - 1);
        }
        loaded++;
    }
    cJSON_Delete(arr);
    *out_loaded = loaded;
    return ESP_OK;
}

static void seed_defaults(void) {
    s_count = 0;
    for (size_t i = 0; i < DEFAULTS_COUNT && s_count < PORTMAP_MAX_ENTRIES; i++) {
        s_entries[s_count++] = DEFAULTS[i];
    }
}

// ─── Listener lifecycle ───────────────────────────────────────
static void apply_listeners(void) {
    // Detener todos los existentes.
    for (int i = 0; i < s_listener_count; i++) {
        proxy_listener_destroy(s_listeners[i]);
        s_listeners[i] = NULL;
    }
    s_listener_count = 0;

    // Arrancar uno por entry. Logueamos fallos pero seguimos con los demás.
    for (int i = 0; i < s_count; i++) {
        proxy_listener_ctx_t *ctx = proxy_listener_create(&s_entries[i]);
        if (ctx) {
            s_listeners[s_listener_count++] = ctx;
        } else {
            ESP_LOGE(TAG, "failed to start listener for :%u (%s)",
                     (unsigned)s_entries[i].local_port, s_entries[i].label);
        }
    }
}

// ─── Public API ───────────────────────────────────────────────
esp_err_t portmap_init(void) {
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "xSemaphoreCreateMutex failed");
        return ESP_ERR_NO_MEM;
    }

    int loaded = 0;
    esp_err_t err = load_from_nvs(&loaded);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no NVS data; seeding defaults");
        seed_defaults();
        save_to_nvs();
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "load_from_nvs failed (%s); seeding defaults",
                 esp_err_to_name(err));
        seed_defaults();
    } else if (loaded == 0) {
        ESP_LOGI(TAG, "NVS had empty array; seeding defaults");
        seed_defaults();
        save_to_nvs();
    } else {
        s_count = loaded;
        ESP_LOGI(TAG, "loaded %d entries from NVS", loaded);
    }

    apply_listeners();
    ESP_LOGI(TAG, "init OK: %d mapping(s) active", s_listener_count);
    return ESP_OK;
}

esp_err_t portmap_add(uint16_t local_port,
                      const char *upstream_host,
                      uint16_t upstream_port,
                      const char *proto,
                      const char *label) {
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    // Validaciones.
    if (local_port < 1024) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_ARG;
    }
    if (upstream_port == 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_ARG;
    }
    if (proto && *proto && strcmp(proto, "http") != 0 && strcmp(proto, "https") != 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < s_count; i++) {
        if (s_entries[i].local_port == local_port) {
            xSemaphoreGive(s_mutex);
            return ESP_ERR_INVALID_STATE;
        }
    }
    if (s_count >= PORTMAP_MAX_ENTRIES) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }

    // Append.
    portmap_entry_t *e = &s_entries[s_count];
    memset(e, 0, sizeof(*e));
    e->local_port    = local_port;
    if (upstream_host) {
        strncpy(e->upstream_host, upstream_host, sizeof(e->upstream_host) - 1);
    }
    e->upstream_port = upstream_port;
    strncpy(e->proto, (proto && *proto) ? proto : "http", sizeof(e->proto) - 1);
    if (label) {
        strncpy(e->label, label, sizeof(e->label) - 1);
    }
    s_count++;

    // Persistir primero; si el bind falla, revertimos.
    esp_err_t err = save_to_nvs();
    if (err != ESP_OK) {
        s_count--;
        xSemaphoreGive(s_mutex);
        return err;
    }

    proxy_listener_ctx_t *ctx = proxy_listener_create(e);
    if (!ctx) {
        s_count--;
        save_to_nvs();  // reescribir sin esta entry
        xSemaphoreGive(s_mutex);
        return ESP_FAIL;
    }
    s_listeners[s_listener_count++] = ctx;

    ESP_LOGI(TAG, "added :%u -> %s:%u (%s) [%s]",
             (unsigned)local_port,
             upstream_host && *upstream_host ? upstream_host : "(auto)",
             (unsigned)upstream_port,
             e->proto, e->label);

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t portmap_del(uint16_t local_port) {
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    int entry_idx = -1;
    for (int i = 0; i < s_count; i++) {
        if (s_entries[i].local_port == local_port) {
            entry_idx = i;
            break;
        }
    }
    if (entry_idx < 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    int listener_idx = -1;
    for (int i = 0; i < s_listener_count; i++) {
        if (s_listeners[i]->entry.local_port == local_port) {
            listener_idx = i;
            break;
        }
    }
    if (listener_idx >= 0) {
        proxy_listener_destroy(s_listeners[listener_idx]);
        for (int i = listener_idx; i < s_listener_count - 1; i++) {
            s_listeners[i] = s_listeners[i + 1];
        }
        s_listeners[--s_listener_count] = NULL;
    }

    for (int i = entry_idx; i < s_count - 1; i++) {
        s_entries[i] = s_entries[i + 1];
    }
    s_count--;

    esp_err_t err = save_to_nvs();
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t portmap_reset_defaults(void) {
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    for (int i = 0; i < s_listener_count; i++) {
        proxy_listener_destroy(s_listeners[i]);
        s_listeners[i] = NULL;
    }
    s_listener_count = 0;

    seed_defaults();
    esp_err_t err = save_to_nvs();
    if (err != ESP_OK) {
        xSemaphoreGive(s_mutex);
        return err;
    }

    apply_listeners();
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

int portmap_count(void) {
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return 0;
    int n = s_count;
    xSemaphoreGive(s_mutex);
    return n;
}

esp_err_t portmap_get_all(portmap_entry_t *out, int max_entries, int *count) {
    if (!out || !count) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    int n = s_count < max_entries ? s_count : max_entries;
    for (int i = 0; i < n; i++) out[i] = s_entries[i];
    *count = n;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

void portmap_format_status(char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    if (s_mutex == NULL) {
        snprintf(out, out_size, "portmap: not initialized");
        return;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        snprintf(out, out_size, "portmap: busy, try again");
        return;
    }
    int p = snprintf(out, out_size,
                      "Port mappings (%d/%d):\n",
                      s_count, PORTMAP_MAX_ENTRIES);
    for (int i = 0; i < s_count && p < (int)out_size - 80; i++) {
        const char *host = s_entries[i].upstream_host[0]
                         ? s_entries[i].upstream_host : "(auto)";
        p += snprintf(out + p, out_size - p,
                      "  :%u -> %s://%s:%u  [%s]\n",
                      (unsigned)s_entries[i].local_port,
                      s_entries[i].proto,
                      host,
                      (unsigned)s_entries[i].upstream_port,
                      s_entries[i].label);
    }
    xSemaphoreGive(s_mutex);
}
