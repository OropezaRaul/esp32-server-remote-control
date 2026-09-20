#include "automation.h"

#include <esp_log.h>
#include <esp_sntp.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include "state.h"
#include "cocopa01_client.h"
#include "telegram.h"
#include "wol.h"
#include "system_health.h"

#include "sdkconfig.h"

static const char *TAG = "automation";

#define NTP_SERVER_1        "pool.ntp.org"
#define NTP_SERVER_2        "time.google.com"
#define GMT_OFFSET_PERU_SEC (-5 * 3600)  // Peru UTC-5, no DST

#define NVS_NAMESPACE        "automation"
#define NVS_KEY_ENABLED      "enabled"
#define NVS_KEY_SUSPEND_MIN  "suspend_min"
#define NVS_KEY_OFF_MIN      "off_min"
#define NVS_KEY_QUIET_START  "quiet_start"
#define NVS_KEY_QUIET_END    "quiet_end"
#define NVS_KEY_LAST_KICK_MS "last_kick_ms"
#define NVS_KEY_OFF_CYCLE    "off_cycle"
#define NVS_KEY_OFF_DEADLINE "off_deadline"

#define CHECK_INTERVAL_S     30
#define KICK_TASK_STACK      8192
#define KICK_TASK_PRIO       4

// Defaults: vienen de Kconfig en tiempo de compilación, pero también se
// usan como fallback si NVS no tiene valores guardados.
static const automation_config_t DEFAULTS = {
    .enabled           = CONFIG_AUTO_DEFAULT_ENABLED,
    .suspend_after_min = CONFIG_AUTO_SUSPEND_AFTER_MIN,
    .off_after_min     = CONFIG_AUTO_OFF_AFTER_MIN,
    .quiet_start_min   = CONFIG_AUTO_QUIET_START_MIN,
    .quiet_end_min     = CONFIG_AUTO_QUIET_END_MIN,
};

static automation_config_t g_cfg;
static volatile uint32_t   g_last_interaction_ms = 0;
static volatile bool       g_initialized         = false;
static volatile bool       g_last_in_quiet       = false;

// Backoff state: cuando el intento de auto-off falla (cocopa01 no wake-up
// desde S3, o /off no llega), NO seguimos reintentando cada 30s. Aplicamos
// backoff exponencial para no spammear y notificar al usuario.
static volatile uint32_t   g_off_last_failure_ms = 0;
static volatile uint32_t   g_off_failure_count   = 0;

// Ciclo interno de auto-off: activo desde que mandamos /suspend (o
// detectamos SUSPEND externo) hasta que el /off termina o se cancela.
// Es INDEPENDIENTE del estado reportado por el poll: el poll degrada
// SUSPEND -> OFF a los 5 min sin respuesta (LAST_ON_HEURISTIC_MS) y eso
// NO debe matar el WOL+/off pendiente. Persistido en NVS para sobrevivir
// al auto-reboot del ESP32 (60 min) a mitad del ciclo.
static volatile bool     g_off_cycle_active   = false;
static volatile uint32_t g_off_deadline_unix  = 0;  // 0 = sin deadline

#define OFF_BACKOFF_BASE_S    300  // 5 min tras 1er fallo
#define OFF_BACKOFF_MAX_S     1800 // 30 min tras varios fallos
#define OFF_BACKOFF_FACTOR    2

// ─── NVS helpers ──────────────────────────────────────────────
static esp_err_t nvs_open_read(nvs_handle_t *out) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return err;
    *out = h;
    return err;
}

static esp_err_t nvs_open_write(nvs_handle_t *out) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open write failed: %s", esp_err_to_name(err));
        return err;
    }
    *out = h;
    return ESP_OK;
}

static void cycle_persist(void) {
    nvs_handle_t h;
    if (nvs_open_write(&h) != ESP_OK) return;
    nvs_set_u8(h,  NVS_KEY_OFF_CYCLE,    g_off_cycle_active ? 1 : 0);
    nvs_set_u32(h, NVS_KEY_OFF_DEADLINE, g_off_deadline_unix);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "off-cycle persisted: active=%d deadline=%lu",
             (int)g_off_cycle_active, (unsigned long)g_off_deadline_unix);
}

static void cycle_clear(void) {
    g_off_cycle_active = false;
    g_off_deadline_unix = 0;
    nvs_handle_t h;
    if (nvs_open_write(&h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_OFF_CYCLE, 0);
    nvs_set_u32(h, NVS_KEY_OFF_DEADLINE, 0);
    nvs_commit(h);
    nvs_close(h);
}

static esp_err_t load_from_nvs(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open_read(&h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no NVS config — using defaults");
        g_cfg = DEFAULTS;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open read failed: %s — using defaults", esp_err_to_name(err));
        g_cfg = DEFAULTS;
        return err;
    }

    g_cfg = DEFAULTS;
    uint8_t u8 = 0;
    uint16_t u16 = 0;
    uint32_t u32 = 0;

    if (nvs_get_u8(h, NVS_KEY_ENABLED, &u8) == ESP_OK) {
        g_cfg.enabled = (u8 != 0);
    }
    if (nvs_get_u16(h, NVS_KEY_SUSPEND_MIN, &u16) == ESP_OK) {
        g_cfg.suspend_after_min = u16;
    }
    if (nvs_get_u16(h, NVS_KEY_OFF_MIN, &u16) == ESP_OK) {
        g_cfg.off_after_min = u16;
    }
    if (nvs_get_u16(h, NVS_KEY_QUIET_START, &u16) == ESP_OK) {
        g_cfg.quiet_start_min = u16;
    }
    if (nvs_get_u16(h, NVS_KEY_QUIET_END, &u16) == ESP_OK) {
        g_cfg.quiet_end_min = u16;
    }
    if (nvs_get_u32(h, NVS_KEY_LAST_KICK_MS, &u32) == ESP_OK) {
        g_last_interaction_ms = u32;
    }
    uint8_t cycle8 = 0;
    uint32_t deadline32 = 0;
    if (nvs_get_u8(h, NVS_KEY_OFF_CYCLE, &cycle8) == ESP_OK && cycle8) {
        g_off_cycle_active = true;
        nvs_get_u32(h, NVS_KEY_OFF_DEADLINE, &deadline32);
        g_off_deadline_unix = deadline32;
        ESP_LOGW(TAG, "resuming off-cycle after boot: deadline_unix=%lu",
                 (unsigned long)g_off_deadline_unix);
    }

    nvs_close(h);
    ESP_LOGI(TAG, "loaded from NVS: en=%d sus=%u off=%u q=[%u..%u) kick=%u cycle=%d",
             (int)g_cfg.enabled,
             (unsigned)g_cfg.suspend_after_min,
             (unsigned)g_cfg.off_after_min,
             (unsigned)g_cfg.quiet_start_min,
             (unsigned)g_cfg.quiet_end_min,
             (unsigned)g_last_interaction_ms,
             (int)g_off_cycle_active);
    return ESP_OK;
}

static esp_err_t save_to_nvs(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open_write(&h);
    if (err != ESP_OK) return err;

    esp_err_t ret = ESP_OK;
    ret |= nvs_set_u8(h,  NVS_KEY_ENABLED,     g_cfg.enabled ? 1 : 0);
    ret |= nvs_set_u16(h, NVS_KEY_SUSPEND_MIN, g_cfg.suspend_after_min);
    ret |= nvs_set_u16(h, NVS_KEY_OFF_MIN,     g_cfg.off_after_min);
    ret |= nvs_set_u16(h, NVS_KEY_QUIET_START, g_cfg.quiet_start_min);
    ret |= nvs_set_u16(h, NVS_KEY_QUIET_END,   g_cfg.quiet_end_min);
    ret |= nvs_set_u32(h, NVS_KEY_LAST_KICK_MS, g_last_interaction_ms);
    if (ret == ESP_OK) {
        ret = nvs_commit(h);
    }
    nvs_close(h);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "save_to_nvs failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "saved to NVS: en=%d sus=%u off=%u q=[%u..%u)",
                 (int)g_cfg.enabled,
                 (unsigned)g_cfg.suspend_after_min,
                 (unsigned)g_cfg.off_after_min,
                 (unsigned)g_cfg.quiet_start_min,
                 (unsigned)g_cfg.quiet_end_min);
    }
    return ret;
}

// ─── Time helpers ─────────────────────────────────────────────
static bool in_quiet_hours(int now_min) {
    int qs = (int)g_cfg.quiet_start_min;
    int qe = (int)g_cfg.quiet_end_min;
    if (qs == qe) return false;
    if (qs < qe) {
        return (now_min >= qs && now_min < qe);
    }
    // wrap midnight (e.g., 22:00..08:00)
    return (now_min >= qs || now_min < qe);
}

static bool get_local_min(int *out_min) {
    time_t now = time(NULL);
    if (now == (time_t)-1 || now < 1704067200) { // pre-2024
        time_t rtc_now = 0;
        if (system_health_restore_rtc_time(&rtc_now)) {
            struct timeval tv = { .tv_sec = rtc_now, .tv_usec = 0 };
            settimeofday(&tv, NULL);
            now = rtc_now;
        } else {
            return false;
        }
    } else {
        system_health_save_rtc_time(now);
    }
    // El ESP-IDF base no tiene tzdata, así que convertimos manualmente a hora
    // de Perú (UTC-5, sin DST) en lugar de pelear con setenv("TZ", ...).
    now += GMT_OFFSET_PERU_SEC;
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);
    // 1970-era tm_year indica que SNTP nunca sincronizó.
    if (timeinfo.tm_year < (2020 - 1900)) return false;
    *out_min = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    return true;
}

static void ntp_init(void) {
    if (esp_sntp_enabled()) {
        ESP_LOGI(TAG, "SNTP already running");
        return;
    }
    ESP_LOGI(TAG, "starting SNTP (%s, %s)", NTP_SERVER_1, NTP_SERVER_2);
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, NTP_SERVER_1);
    esp_sntp_setservername(1, NTP_SERVER_2);
    esp_sntp_set_sync_interval(6UL * 3600 * 1000);  // re-sync cada 6h
    esp_sntp_init();
}

// ─── Public API ───────────────────────────────────────────────
void automation_kick(void) {
    if (!g_initialized) return;
    g_last_interaction_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    ESP_LOGD(TAG, "kick — last_interaction_ms=%u", (unsigned)g_last_interaction_ms);

    if (g_off_cycle_active) {
        ESP_LOGI(TAG, "kick received while off-cycle active -> cancelling off-cycle");
        cycle_clear();
    }

    // Persistir el kick también permite sobrevivir reboots: si cocopa01
    // sigue ON tras un reboot del ESP32, no queremos suspend inmediato
    // porque el usuario estuvo activo hace 1 minuto.
    nvs_handle_t h;
    if (nvs_open_write(&h) == ESP_OK) {
        nvs_set_u32(h, NVS_KEY_LAST_KICK_MS, g_last_interaction_ms);
        nvs_commit(h);
        nvs_close(h);
    }
}

void automation_get_config(automation_config_t *out) {
    if (out == NULL) return;
    *out = g_cfg;
}

esp_err_t automation_set_config(const automation_config_t *cfg) {
    if (cfg == NULL) {
        g_cfg = DEFAULTS;
    } else {
        g_cfg = *cfg;
    }
    return save_to_nvs();
}

esp_err_t automation_reset_defaults(void) {
    g_cfg = DEFAULTS;
    return save_to_nvs();
}

bool automation_is_enabled(void) {
    return g_cfg.enabled;
}

int automation_parse_hhmm(const char *s) {
    if (s == NULL) return -1;
    while (*s && isspace((unsigned char)*s)) s++;
    if (!isdigit((unsigned char)*s)) return -1;

    int h = 0, m = 0;
    int n = 0;
    while (isdigit((unsigned char)*s) && n < 3) {
        h = h * 10 + (*s - '0');
        s++; n++;
    }
    if (h < 0 || h > 23) return -1;
    if (*s != ':') return -1;
    s++;
    n = 0;
    while (isdigit((unsigned char)*s) && n < 3) {
        m = m * 10 + (*s - '0');
        s++; n++;
    }
    if (m < 0 || m > 59) return -1;
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s != 0) return -1;
    return h * 60 + m;
}

static void format_min(uint16_t min_of_day, char *out, size_t out_size) {
    snprintf(out, out_size, "%02u:%02u", min_of_day / 60, min_of_day % 60);
}

void automation_status_str(char *out, size_t out_size) {
    if (out == NULL || out_size == 0) return;
    char qs[8], qe[8];
    format_min(g_cfg.quiet_start_min, qs, sizeof(qs));
    format_min(g_cfg.quiet_end_min,   qe, sizeof(qe));

    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    uint32_t idle_ms = (g_last_interaction_ms > 0 && now_ms >= g_last_interaction_ms)
                     ? (now_ms - g_last_interaction_ms) : 0;
    uint32_t idle_min = idle_ms / 60000UL;

    cocopa01_state_t st = state_get();

    int p = snprintf(out, out_size,
        "Automation: %s\n"
        "  suspend after: %u min inactivity\n"
        "  off after:     %u min post-suspend\n"
        "  quiet hours:   %s - %s (Peru UTC-5)\n"
        "  state: %s\n"
        "  last interaction: %u min ago",
        g_cfg.enabled ? "ENABLED" : "DISABLED",
        (unsigned)g_cfg.suspend_after_min,
        (unsigned)g_cfg.off_after_min,
        qs, qe,
        cocopa01_state_str(st),
        (unsigned)idle_min);

    if (g_cfg.enabled && st == COCOPA01_STATE_ON && g_cfg.suspend_after_min > 0) {
        uint32_t left = (idle_min < g_cfg.suspend_after_min)
                      ? (g_cfg.suspend_after_min - idle_min) : 0;
        p += snprintf(out + p, out_size - p,
            "\n  next action: suspend in %u min", (unsigned)left);
    } else if (g_cfg.enabled && st == COCOPA01_STATE_SUSPEND && g_cfg.off_after_min > 0) {
        uint32_t left = (idle_min < g_cfg.off_after_min)
                      ? (g_cfg.off_after_min - idle_min) : 0;
        int now_min_local = -1;
        bool quiet = !get_local_min(&now_min_local) || in_quiet_hours(now_min_local);
        p += snprintf(out + p, out_size - p,
            "\n  next action: off in %u min%s",
            (unsigned)left,
            quiet ? " (SUPPRESSED — quiet hours or NTP unsynced)" : "");
    }

    (void)p;
}

// ─── Task ─────────────────────────────────────────────────────
static void notify_authorized(const char *text) {
    if (text == NULL || *text == 0) return;
    // Reusamos la API pública: telegram_send_text a un chat_id. Como
    // automation.c no conoce los IDs autorizados (privados en telegram.c),
    // usamos telegram_broadcast — función agregada en telegram.h.
    telegram_broadcast(text);
}

static void automation_task(void *arg) {
    ESP_LOGI(TAG, "task started: enabled=%d sus=%u off=%u q=[%u..%u)",
             (int)g_cfg.enabled,
             (unsigned)g_cfg.suspend_after_min,
             (unsigned)g_cfg.off_after_min,
             (unsigned)g_cfg.quiet_start_min,
             (unsigned)g_cfg.quiet_end_min);

    // Si el NVS tenía un last_kick_ms y la diferencia es razonable (< 24h),
    // mantenerlo. Si pasó más de un día, forzar kick (no queremos
    // suspend inmediato post-reboot).
    uint32_t boot_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (g_last_interaction_ms == 0 ||
        (boot_ms - g_last_interaction_ms) > 24UL * 3600UL * 1000UL) {
        g_last_interaction_ms = boot_ms;
    }

    while (1) {
        system_health_heartbeat(HEALTH_CHANNEL_AUTOMATION);
        vTaskDelay(pdMS_TO_TICKS(CHECK_INTERVAL_S * 1000));
        system_health_heartbeat(HEALTH_CHANNEL_AUTOMATION);

        if (!g_cfg.enabled) continue;

        cocopa01_state_t st = state_get();

        // Si el poll degradó SUSPEND -> OFF (5 min sin respuesta del S3) pero
        // nuestro ciclo de auto-off sigue activo, NO abandonar: seguimos en el
        // brazo de off. El estado reportado pasa a OFF pero el server físico
        // sigue en S3. Antes moría aquí y el /off nunca se enviaba.
        bool in_off_cycle = g_off_cycle_active;

        if (st == COCOPA01_STATE_OFF && in_off_cycle) {
            ESP_LOGD(TAG, "state=OFF but off-cycle active (poll degraded SUSPEND->OFF), continuing off branch");
        }

        if (st != COCOPA01_STATE_ON && st != COCOPA01_STATE_SUSPEND && !in_off_cycle) {
            // OFF real sin ciclo o UNKNOWN: nada que temporizar.
            continue;
        }

        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        uint32_t idle_ms = now_ms - g_last_interaction_ms;
        uint32_t idle_min = idle_ms / 60000UL;

        if (st == COCOPA01_STATE_ON) {
            // El server está ON: si veníamos de un ciclo de off (p.ej. WOL
            // del brazo de off lo despertó, o alguien lo encendió), el ciclo
            // ya no aplica salvo que sea el nuestro. El brazo de off maneja
            // su propio WOL dentro de la misma iteración; al volver aquí con
            // ON y ciclo activo, lo cancelamos (fue despertado).
            if (in_off_cycle) {
                ESP_LOGI(TAG, "off-cycle cancelled: cocopa01 is ON");
                cycle_clear();
            }
            if (g_cfg.suspend_after_min == 0) continue;
            if (idle_min < g_cfg.suspend_after_min) continue;

            ESP_LOGI(TAG,
                "ACTION: idle %u min >= %u min threshold. POST /suspend to cocopa01:%d",
                (unsigned)idle_min, (unsigned)g_cfg.suspend_after_min,
                CONFIG_COCOPA01_HTTP_PORT);
            char msg[160];
            snprintf(msg, sizeof(msg),
                "Automation: cocopa01 idle %u min. Sending /suspend.",
                (unsigned)idle_min);
            notify_authorized(msg);

            esp_err_t sres = cocopa01_send_suspend();
            if (sres == ESP_OK) {
                ESP_LOGI(TAG, "ACTION: /suspend OK, state -> SUSPEND, off-timer reset");
                state_set(COCOPA01_STATE_SUSPEND);
                g_last_interaction_ms = now_ms;  // arranca el segundo timer
                g_last_in_quiet = false;
                // Activar ciclo interno de auto-off: inmune a la degradacion
                // SUSPEND->OFF del poll y persistido (sobrevive reboots).
                if (g_cfg.off_after_min > 0) {
                    g_off_cycle_active = true;
                    g_off_deadline_unix = (uint32_t)time(NULL)
                                        + (uint32_t)g_cfg.off_after_min * 60U;
                    cycle_persist();
                }
            } else {
                ESP_LOGW(TAG, "ACTION: /suspend failed (%s), will retry next cycle",
                         esp_err_to_name(sres));
            }
        } else {
            // COCOPA01_STATE_SUSPEND (o OFF con ciclo activo tras degradacion)
            if (!in_off_cycle && st == COCOPA01_STATE_OFF) continue;  // OFF real

            // Estado SUSPEND observado sin ciclo propio (p.ej. /suspend manual
            // por Telegram): adoptarlo como ciclo de auto-off.
            if (!in_off_cycle && st == COCOPA01_STATE_SUSPEND && g_cfg.off_after_min > 0) {
                ESP_LOGI(TAG, "adopting observed SUSPEND as off-cycle");
                g_off_cycle_active = true;
                g_off_deadline_unix = (uint32_t)time(NULL)
                                    + (uint32_t)g_cfg.off_after_min * 60U;
                cycle_persist();
                in_off_cycle = true;
            }

            if (g_cfg.off_after_min == 0) continue;

            // Disparo por tiempo: idle >= off_after_min, O deadline Unix
            // alcanzado (cubre el caso en que el ESP32 se reinició a mitad
            // del ciclo y el contador idle monótono se perdió).
            uint32_t now_unix = (uint32_t)time(NULL);
            bool deadline_due = (g_off_deadline_unix != 0 && now_unix >= g_off_deadline_unix);
            if (idle_min < g_cfg.off_after_min && !deadline_due) continue;

            // Backoff: si un intento anterior falló, esperamos antes de reintentar.
            if (g_off_last_failure_ms != 0) {
                uint32_t since_ms = now_ms - g_off_last_failure_ms;
                uint32_t cooldown_ms = OFF_BACKOFF_BASE_S * 1000UL;
                for (uint32_t i = 1; i < g_off_failure_count && cooldown_ms < (uint32_t)OFF_BACKOFF_MAX_S * 1000UL; i++) {
                    cooldown_ms *= OFF_BACKOFF_FACTOR;
                }
                if (cooldown_ms > (uint32_t)OFF_BACKOFF_MAX_S * 1000UL) {
                    cooldown_ms = OFF_BACKOFF_MAX_S * 1000UL;
                }
                if (since_ms < cooldown_ms) {
                    ESP_LOGD(TAG, "off-action in backoff (%u s / %u s remaining, %u failures)",
                             (unsigned)(since_ms / 1000),
                             (unsigned)((cooldown_ms - since_ms) / 1000),
                             (unsigned)g_off_failure_count);
                    continue;
                }
            }

            int now_min_local = -1;
            bool have_time = get_local_min(&now_min_local);
            bool quiet = !have_time || in_quiet_hours(now_min_local);

            if (have_time && quiet != g_last_in_quiet) {
                ESP_LOGI(TAG, "quiet hours state changed: %s",
                         quiet ? "ENTERED" : "EXITED");
                g_last_in_quiet = quiet;
            }

            if (quiet) {
                ESP_LOGI(TAG,
                    "idle %u min >= %u min but in quiet hours (or NTP unsynced) — skipping /off",
                    (unsigned)idle_min, (unsigned)g_cfg.off_after_min);
                continue;
            }

            ESP_LOGI(TAG,
                "ACTION: suspended %u min >= %u min, outside quiet hours. WOL + /off",
                (unsigned)idle_min, (unsigned)g_cfg.off_after_min);
            char msg[200];
            snprintf(msg, sizeof(msg),
                "Automation: cocopa01 suspended for %u min (outside quiet hours). Waking + /off.",
                (unsigned)idle_min);
            notify_authorized(msg);

            // cocopa01 in S3 has NIC down, so /off can't reach it.
            // ACPI doesn't allow S3->S5 directly; we have to go S3 -> S0 (WOL)
            // -> S5 (/off). Same dance as the manual /off handler.
            ESP_LOGI(TAG, "ACTION: WOL to wake cocopa01 from S3 before /off");
            esp_err_t wres = wol_send_default();
            if (wres != ESP_OK) {
                g_off_last_failure_ms = now_ms;
                g_off_failure_count++;
                ESP_LOGW(TAG, "ACTION: WOL send failed (%s), backoff %u",
                         esp_err_to_name(wres), (unsigned)g_off_failure_count);
                char em[160];
                snprintf(em, sizeof(em),
                    "Automation: WOL failed (%s). Retrying in %u min (attempt %u).",
                    esp_err_to_name(wres),
                    (unsigned)(OFF_BACKOFF_BASE_S / 60),
                    (unsigned)g_off_failure_count);
                notify_authorized(em);
                continue;
            }
            state_mark_known_on();
            // Wait for cocopa01 to come back. S3 resume is fast (~5-10s),
            // but BIOS + userspace can take up to 60s.
            bool came_up = false;
            for (int i = 0; i < 30; i++) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                if (cocopa01_tcp_alive(2000)) {
                    came_up = true;
                    break;
                }
            }
            if (!came_up) {
                g_off_last_failure_ms = now_ms;
                g_off_failure_count++;
                uint32_t backoff_s = OFF_BACKOFF_BASE_S;
                for (uint32_t i = 1; i < g_off_failure_count && backoff_s < (uint32_t)OFF_BACKOFF_MAX_S; i++) {
                    backoff_s *= OFF_BACKOFF_FACTOR;
                }
                if (backoff_s > (uint32_t)OFF_BACKOFF_MAX_S) backoff_s = OFF_BACKOFF_MAX_S;
                ESP_LOGW(TAG,
                    "ACTION: WOL sent but cocopa01 didn't come back in 60s "
                    "(attempt %u). Backoff %u s.",
                    (unsigned)g_off_failure_count, (unsigned)backoff_s);
                char em[200];
                snprintf(em, sizeof(em),
                    "Automation: cocopa01 didn't wake from S3 after WOL (attempt %u). "
                    "Retrying in %u min. Check BIOS WOL if persistent.",
                    (unsigned)g_off_failure_count,
                    (unsigned)(backoff_s / 60));
                notify_authorized(em);
                continue;
            }
            ESP_LOGI(TAG, "ACTION: cocopa01 back in S0, sending /off");

            esp_err_t ores = cocopa01_send_off();
            if (ores == ESP_OK) {
                ESP_LOGI(TAG, "ACTION: /off OK, state -> OFF, backoff counter cleared");
                g_off_last_failure_ms = 0;
                g_off_failure_count  = 0;
                cycle_clear();
                state_set(COCOPA01_STATE_OFF);
            } else {
                g_off_last_failure_ms = now_ms;
                g_off_failure_count++;
                ESP_LOGW(TAG, "ACTION: /off failed (%s), backoff %u",
                         esp_err_to_name(ores), (unsigned)g_off_failure_count);
                char em[160];
                snprintf(em, sizeof(em),
                    "Automation: /off failed after waking cocopa01 (%s). Retrying in %u min.",
                    esp_err_to_name(ores),
                    (unsigned)(OFF_BACKOFF_BASE_S / 60));
                notify_authorized(em);
            }
        }
    }
}

esp_err_t automation_init(void) {
    g_cfg = DEFAULTS;
    g_last_interaction_ms = 0;

    ntp_init();

    esp_err_t err = load_from_nvs();
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "load_from_nvs returned %s — continuing with whatever was loaded",
                 esp_err_to_name(err));
    }

    g_last_interaction_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    BaseType_t ok = xTaskCreate(automation_task, "automation",
                                KICK_TASK_STACK, NULL, KICK_TASK_PRIO, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed");
        return ESP_FAIL;
    }

    g_initialized = true;
    ESP_LOGI(TAG, "init OK");
    return ESP_OK;
}
