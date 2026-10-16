#include "scheduler.h"

#include <esp_log.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <stdio.h>

#include "state.h"
#include "server_client.h"
#include "telegram.h"
#include "wol.h"
#include "wake.h"
#include "system_health.h"

static const char *TAG = "scheduler";

#define GMT_OFFSET_PERU_SEC (-5 * 3600)  // Perú UTC-5, sin DST (igual que automation.c)

#define CHECK_INTERVAL_S 30
// Ventana (minutos) tras la hora objetivo en la que todavía se dispara.
// Ampliado a 60 min para cubrir reboots, caídas temporales de red o demora
// de SNTP: si el ESP32 o el router estuvieron caídos a la hora objetivo,
// al recuperarse dentro de la primera hora igual disparará el wake.
#define FIRE_GRACE_MIN   60

#define TASK_STACK 8192
#define TASK_PRIO  3

#define NVS_NAMESPACE "schedule"
#define NVS_KEY_WAKE_MIN "wake_min"
#define NVS_KEY_OFF_MIN  "off_min"
#define NVS_KEY_WAKE_DAY "wake_day"
#define NVS_KEY_OFF_DAY  "off_day"

#define MIN_UNSET 0xFFFF

static scheduler_config_t g_cfg;
static volatile bool g_initialized = false;

// Flag de "NVS fue borrada en este boot". Lo setea main.c apenas detecta
// ESP_ERR_NVS_NO_FREE_PAGES o ESP_ERR_NVS_NEW_VERSION_FOUND, antes de
// ejecutar nvs_flash_erase(). El telegram_poll_task lo lee con
// scheduler_consume_nvs_wiped() y avisa al usuario una sola vez por boot.
// volatile porque lo escribe main() (otro task conceptualmente) y lo
// leen scheduler_task + telegram_poll_task.
static volatile bool g_nvs_wiped = false;

// Día local (unix_s + offset) / 86400 del último disparo de cada acción.
// Evita disparar dos veces dentro de la ventana de gracia del mismo día.
// Se persisten para que un reboot del ESP32 dentro de la ventana no
// re-dispare (ej. watchdog de Tailscale reboota 8:05 → no repetir WOL).
static uint32_t g_wake_day = 0;
static uint32_t g_off_day  = 0;

// ─── NVS ──────────────────────────────────────────────────────
static esp_err_t load_from_nvs(void) {
    g_cfg.wake_set = false;
    g_cfg.off_set  = false;
    g_cfg.wake_min = MIN_UNSET;
    g_cfg.off_min  = MIN_UNSET;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no NVS config — scheduler vacío (sin wake/off)");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open read failed: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t u16 = 0;
    uint32_t u32 = 0;
    if (nvs_get_u16(h, NVS_KEY_WAKE_MIN, &u16) == ESP_OK && u16 != MIN_UNSET) {
        if (u16 < 1440) {
            g_cfg.wake_min = u16;
            g_cfg.wake_set = true;
        }
    }
    if (nvs_get_u16(h, NVS_KEY_OFF_MIN, &u16) == ESP_OK && u16 != MIN_UNSET) {
        if (u16 < 1440) {
            g_cfg.off_min = u16;
            g_cfg.off_set = true;
        }
    }
    if (nvs_get_u32(h, NVS_KEY_WAKE_DAY, &u32) == ESP_OK) g_wake_day = u32;
    if (nvs_get_u32(h, NVS_KEY_OFF_DAY, &u32) == ESP_OK)  g_off_day  = u32;

    nvs_close(h);
    ESP_LOGI(TAG, "loaded from NVS: wake=%s off=%s wake_day=%lu off_day=%lu",
             g_cfg.wake_set ? "set" : "unset",
             g_cfg.off_set ? "set" : "unset",
             (unsigned long)g_wake_day, (unsigned long)g_off_day);
    return ESP_OK;
}

static esp_err_t persist_days(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    esp_err_t ret = ESP_OK;
    ret |= nvs_set_u32(h, NVS_KEY_WAKE_DAY, g_wake_day);
    ret |= nvs_set_u32(h, NVS_KEY_OFF_DAY, g_off_day);
    if (ret == ESP_OK) ret = nvs_commit(h);
    nvs_close(h);
    return ret;
}

// ─── Hora local Perú ──────────────────────────────────────────
static bool get_local_time(int *out_min, uint32_t *out_day) {
    time_t now = time(NULL);
    if (now == (time_t)-1 || now < 1704067200) { // pre-2024
        // Intentar restaurar desde memoria RTC si SNTP aún no sincronizó
        time_t rtc_now = 0;
        if (system_health_restore_rtc_time(&rtc_now)) {
            struct timeval tv = { .tv_sec = rtc_now, .tv_usec = 0 };
            settimeofday(&tv, NULL);
            now = rtc_now;
            ESP_LOGI(TAG, "restored wall time from RTC memory: %ld", (long)now);
        } else {
            return false;
        }
    } else {
        // Hora válida de SNTP: respaldar en RTC para sobrevivir reboots
        system_health_save_rtc_time(now);
    }
    now += GMT_OFFSET_PERU_SEC;
    struct tm ti;
    gmtime_r(&now, &ti);
    if (ti.tm_year < (2020 - 1900)) return false;
    *out_min = ti.tm_hour * 60 + ti.tm_min;
    *out_day = (uint32_t)((int64_t)now / 86400);
    return true;
}

static bool due(uint16_t target_min, uint32_t last_day, int now_min,
                uint32_t today) {
    if (now_min < (int)target_min) return false;
    if (now_min >= (int)target_min + FIRE_GRACE_MIN) return false;
    return last_day != today;
}

// ─── Acciones ─────────────────────────────────────────────────
static void fire_wake(void) {
    if (state_get() == SERVER_STATE_ON) {
        ESP_LOGI(TAG, "wake programado: server ya está ON — skip");
        telegram_broadcast("Scheduler: hora de wake — server ya estaba ON, nada que hacer.");
        return;
    }
    esp_err_t wres = wake_request("scheduler");
    if (wres == ESP_OK) {
        state_mark_known_on();
        ESP_LOGI(TAG, "wake programado: WOL enviado (con reintentos)");
        telegram_broadcast("Scheduler: wake programado — magic packet enviado a server "
                           "(reintento automatico cada 10 s hasta 4 min).");
    } else {
        ESP_LOGW(TAG, "wake programado: WOL falló (%s)", esp_err_to_name(wres));
        char em[160];
        snprintf(em, sizeof(em),
                 "Scheduler: wake programado FALLÓ al enviar WOL (%s).",
                 esp_err_to_name(wres));
        telegram_broadcast(em);
    }
}

static void fire_off(void) {
    if (server_tcp_alive(2000)) {
        if (server_send_off() == ESP_OK) {
            state_set(SERVER_STATE_OFF);
            ESP_LOGI(TAG, "off programado: /off OK");
            telegram_broadcast("Scheduler: off programado ejecutado — server OFF.");
        } else {
            ESP_LOGW(TAG, "off programado: POST /off rechazado");
            telegram_broadcast("Scheduler: off programado FALLÓ (server rechazó /off).");
        }
        return;
    }

    // En S3 la NIC está caída: ACPI no permite S3→S5 directo, hay que
    // despertar a S0 con WOL y recién ahí mandar /off. Mismo baile que el
    // /off manual (telegram.c) y que automation.c.
    if (state_get() == SERVER_STATE_SUSPEND) {
        ESP_LOGI(TAG, "off programado: server en S3 — WOL para poder apagar");
        if (wol_send_default() != ESP_OK) {
            telegram_broadcast("Scheduler: off programado FALLÓ (no se pudo enviar WOL).");
            return;
        }
        state_mark_known_on();
        bool came_up = false;
        for (int i = 0; i < 30; i++) {  // hasta 60s: BIOS+userspace pueden tardar
            vTaskDelay(pdMS_TO_TICKS(2000));
            if (server_tcp_alive(2000)) { came_up = true; break; }
        }
        if (!came_up) {
            ESP_LOGW(TAG, "off programado: server no volvió tras WOL en 60s");
            telegram_broadcast("Scheduler: off programado omitido (server no despertó tras WOL).");
            return;
        }
        if (server_send_off() == ESP_OK) {
            state_set(SERVER_STATE_OFF);
            ESP_LOGI(TAG, "off programado: S3→S0→S5 completo");
            telegram_broadcast("Scheduler: off programado — server despertado de S3 y apagado.");
        } else {
            telegram_broadcast("Scheduler: off programado FALLÓ (rechazó /off tras despertar).");
        }
        return;
    }

    // OFF o UNKNOWN sin TCP: no hay nada que apagar (o no accesible).
    ESP_LOGI(TAG, "off programado: server no accesible — skip");
    telegram_broadcast("Scheduler: hora de off — server no accesible (ya apagado?), nada que hacer.");
}

// ─── Task ─────────────────────────────────────────────────────
static void scheduler_task(void *arg) {
    ESP_LOGI(TAG, "task started: wake=%s off=%s (grace=%u min, check=%us)",
             g_cfg.wake_set ? "set" : "unset",
             g_cfg.off_set ? "set" : "unset",
             (unsigned)FIRE_GRACE_MIN, (unsigned)CHECK_INTERVAL_S);

    while (1) {
        system_health_heartbeat(HEALTH_CHANNEL_SCHEDULER);
        vTaskDelay(pdMS_TO_TICKS(CHECK_INTERVAL_S * 1000));
        system_health_heartbeat(HEALTH_CHANNEL_SCHEDULER);

        int now_min = -1;
        uint32_t today = 0;
        if (!get_local_time(&now_min, &today)) continue;  // SNTP sin sync

        bool dirty = false;

        if (g_cfg.wake_set && due(g_cfg.wake_min, g_wake_day, now_min, today)) {
            g_wake_day = today;
            persist_days();
            fire_wake();
            dirty = true;
        }
        if (g_cfg.off_set && due(g_cfg.off_min, g_off_day, now_min, today)) {
            g_off_day = today;
            persist_days();
            fire_off();
            dirty = true;
        }
        (void)dirty;
    }
}

// ─── API pública ──────────────────────────────────────────────
esp_err_t scheduler_init(void) {
    esp_err_t err = load_from_nvs();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "load_from_nvs: %s — arranco sin horarios", esp_err_to_name(err));
    }

    BaseType_t ok = xTaskCreate(scheduler_task, "schedule",
                                TASK_STACK, NULL, TASK_PRIO, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed");
        return ESP_FAIL;
    }

    g_initialized = true;
    ESP_LOGI(TAG, "init OK");
    return ESP_OK;
}

esp_err_t scheduler_set(const scheduler_config_t *cfg) {
    if (cfg == NULL) return scheduler_reset();

    scheduler_config_t clean = *cfg;
    if (clean.wake_min >= 1440) { clean.wake_set = false; clean.wake_min = MIN_UNSET; }
    if (clean.off_min  >= 1440) { clean.off_set  = false; clean.off_min  = MIN_UNSET; }
    if (!clean.wake_set) clean.wake_min = MIN_UNSET;
    if (!clean.off_set)  clean.off_min  = MIN_UNSET;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open write failed: %s", esp_err_to_name(err));
        return err;
    }
    esp_err_t ret = ESP_OK;
    ret |= nvs_set_u16(h, NVS_KEY_WAKE_MIN, clean.wake_min);
    ret |= nvs_set_u16(h, NVS_KEY_OFF_MIN,  clean.off_min);
    if (ret == ESP_OK) ret = nvs_commit(h);
    nvs_close(h);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "scheduler_set persist failed: %s", esp_err_to_name(ret));
        return ret;
    }

    g_cfg = clean;
    // Resetear dedup: un horario recién seteado dentro de su ventana debe
    // disparar (persist_days guardará los días viejos hasta el fire).
    ESP_LOGI(TAG, "config actualizada: wake=%s off=%s",
             g_cfg.wake_set ? "set" : "unset",
             g_cfg.off_set ? "set" : "unset");
    return ESP_OK;
}

void scheduler_get(scheduler_config_t *out) {
    if (out == NULL) return;
    *out = g_cfg;
}

esp_err_t scheduler_reset(void) {
    scheduler_config_t empty = { .wake_set = false, .wake_min = MIN_UNSET,
                                 .off_set = false,  .off_min = MIN_UNSET };
    return scheduler_set(&empty);
}

esp_err_t scheduler_clear_wake(void) {
    scheduler_config_t cfg;
    scheduler_get(&cfg);
    cfg.wake_set = false;
    cfg.wake_min = MIN_UNSET;
    return scheduler_set(&cfg);
}

void scheduler_status_str(char *out, size_t out_size) {
    if (out == NULL || out_size == 0) return;

    char wake[8] = "none";
    char off[8]  = "none";
    if (g_cfg.wake_set) snprintf(wake, sizeof(wake), "%02u:%02u",
                                 (unsigned)(g_cfg.wake_min / 60),
                                 (unsigned)(g_cfg.wake_min % 60));
    if (g_cfg.off_set)  snprintf(off, sizeof(off), "%02u:%02u",
                                 (unsigned)(g_cfg.off_min / 60),
                                 (unsigned)(g_cfg.off_min % 60));

    int now_min = -1;
    uint32_t today = 0;
    bool synced = get_local_time(&now_min, &today);

    int p = snprintf(out, out_size,
        "Schedule (hora Perú UTC-5, diario):\n"
        "  wake: %s\n"
        "  off:  %s\n"
        "  SNTP: %s",
        wake, off,
        synced ? "synced" : "NOT synced (no dispara hasta sync)");

    if (synced) {
        p += snprintf(out + p, out_size - p, "\n  hora actual: %02d:%02d",
                      now_min / 60, now_min % 60);
    }
    (void)p;
}

// ─── NVS-wiped flag (API pública) ──────────────────────────────
// main.c llama scheduler_mark_nvs_wiped() ANTES de nvs_flash_erase() para
// que el resto del firmware sepa que la NVS fue (o va a ser) borrada. El
// telegram_poll_task lo consume con scheduler_consume_nvs_wiped() y manda
// un único aviso por boot a los chats autorizados.

void scheduler_mark_nvs_wiped(void) {
    g_nvs_wiped = true;
}

bool scheduler_nvs_was_wiped(void) {
    return g_nvs_wiped;
}

bool scheduler_consume_nvs_wiped(void) {
    // No atómico bajo IRQ, pero solo lo llaman tareas FreeRTOS — basta con
    // un set/clear no interrumpido por otro consumidor. Si dos tareas
    // llaman a la vez, ambas ven true; telegram_broadcast es idempotente
    // (los 2 chats reciben el mismo texto y el operador lo borra).
    if (g_nvs_wiped) {
        g_nvs_wiped = false;
        return true;
    }
    return false;
}
