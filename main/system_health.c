#include "system_health.h"

#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/time.h>
#include <esp_psram.h>

static const char *TAG = "sys_health";

#define RTC_HEALTH_MAGIC 0x53595354 // 'SYST'
#define SUPERVISOR_CHECK_INTERVAL_MS 3000
#define SUPERVISOR_TASK_STACK 4096
#define SUPERVISOR_TASK_PRIO  6

#define RTC_CRASH_LOG_SIZE 3072
#define LIVE_LOG_BUF_SIZE  (64 * 1024)

// RTC Fast Memory: sobrevive a esp_restart(), WDT reset y software crashes
typedef struct {
    uint32_t magic;
    uint32_t reset_count;
    uint32_t prev_uptime_s;
    health_channel_t failing_channel;
    char     failing_task[32];
    char     reason[64];
    bool     had_crash;
    time_t   rtc_unix_sec;          // Última hora UTC conocida
    int64_t  rtc_save_esp_timer_us; // esp_timer_get_time() cuando se guardó
    char     crash_log[RTC_CRASH_LOG_SIZE];
    uint32_t crash_log_head;
    uint32_t crash_log_len;
} rtc_health_data_t;

RTC_NOINIT_ATTR static rtc_health_data_t s_rtc_data;

static char *s_live_log_buf = NULL;
static uint32_t s_live_log_head = 0;
static uint32_t s_live_log_len = 0;
static portMUX_TYPE s_log_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_default_vprintf = NULL;
static volatile bool s_log_capture_ready = false;

static int health_vprintf(const char *fmt, va_list args) {
    if (!s_log_capture_ready) {
        if (s_default_vprintf != NULL) return s_default_vprintf(fmt, args);
        return vprintf(fmt, args);
    }

    char temp[256];
    va_list copy;
    va_copy(copy, args);
    int len = vsnprintf(temp, sizeof(temp), fmt, copy);
    va_end(copy);

    if (len > 0) {
        size_t to_write = (len < (int)sizeof(temp)) ? (size_t)len : (sizeof(temp) - 1);

        portENTER_CRITICAL(&s_log_mux);
        // 1. Live log buffer in PSRAM
        if (s_live_log_buf != NULL) {
            if (s_live_log_head >= LIVE_LOG_BUF_SIZE) s_live_log_head = 0;
            for (size_t i = 0; i < to_write; i++) {
                s_live_log_buf[s_live_log_head] = temp[i];
                s_live_log_head = (s_live_log_head + 1) % LIVE_LOG_BUF_SIZE;
                if (s_live_log_len < LIVE_LOG_BUF_SIZE) {
                    s_live_log_len++;
                }
            }
        }
        // 2. Crash log buffer in RTC memory
        if (s_rtc_data.magic == RTC_HEALTH_MAGIC) {
            if (s_rtc_data.crash_log_head >= RTC_CRASH_LOG_SIZE) s_rtc_data.crash_log_head = 0;
            for (size_t i = 0; i < to_write; i++) {
                s_rtc_data.crash_log[s_rtc_data.crash_log_head] = temp[i];
                s_rtc_data.crash_log_head = (s_rtc_data.crash_log_head + 1) % RTC_CRASH_LOG_SIZE;
                if (s_rtc_data.crash_log_len < RTC_CRASH_LOG_SIZE) {
                    s_rtc_data.crash_log_len++;
                }
            }
        }
        portEXIT_CRITICAL(&s_log_mux);
    }

    if (s_default_vprintf != NULL) {
        return s_default_vprintf(fmt, args);
    }
    return vprintf(fmt, args);
}

typedef struct {
    const char *name;
    uint32_t   max_interval_ms;
    volatile uint32_t last_heartbeat_ms;
    volatile bool     enabled;
} channel_desc_t;

static channel_desc_t s_channels[HEALTH_CHANNEL_COUNT] = {
    [HEALTH_CHANNEL_SCHEDULER]   = { .name = "scheduler",      .max_interval_ms = 90000, .last_heartbeat_ms = 0, .enabled = false },
    [HEALTH_CHANNEL_AUTOMATION]  = { .name = "automation",     .max_interval_ms = 90000, .last_heartbeat_ms = 0, .enabled = false },
    [HEALTH_CHANNEL_WIFI]        = { .name = "wifi_keepalive", .max_interval_ms = 45000, .last_heartbeat_ms = 0, .enabled = false },
    [HEALTH_CHANNEL_STATE]       = { .name = "state_poll",     .max_interval_ms = 90000, .last_heartbeat_ms = 0, .enabled = false },
    [HEALTH_CHANNEL_TELEGRAM]    = { .name = "telegram_poll",  .max_interval_ms = 90000, .last_heartbeat_ms = 0, .enabled = false },
    [HEALTH_CHANNEL_AUTO_REBOOT] = { .name = "auto_reboot",    .max_interval_ms = 60000, .last_heartbeat_ms = 0, .enabled = false },
    // La secuencia de apagado puede tardar hasta ~100 s (WOL + espera + /off):
    // el limite es holgado para que el supervisor reinicie el ESP32 solo si la
    // task se cuelga de verdad. La task desactiva el canal al terminar.
    [HEALTH_CHANNEL_POWER_OFF]   = { .name = "power_off",      .max_interval_ms = 200000, .last_heartbeat_ms = 0, .enabled = false },
};

static esp_task_wdt_user_handle_t s_twdt_user = NULL;
static bool s_initialized = false;

static uint32_t now_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

void system_health_heartbeat(health_channel_t channel) {
    if ((int)channel < 0 || channel >= HEALTH_CHANNEL_COUNT) return;
    s_channels[channel].last_heartbeat_ms = now_ms();
    s_channels[channel].enabled = true;
}

void system_health_enable_channel(health_channel_t channel, bool enable) {
    if ((int)channel < 0 || channel >= HEALTH_CHANNEL_COUNT) return;
    s_channels[channel].enabled = enable;
    if (enable) {
        s_channels[channel].last_heartbeat_ms = now_ms();
    }
}

static void supervisor_task(void *arg) {
    ESP_LOGI(TAG, "Supervisor task started (priority %d, check interval %u ms)",
             SUPERVISOR_TASK_PRIO, (unsigned)SUPERVISOR_CHECK_INTERVAL_MS);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(SUPERVISOR_CHECK_INTERVAL_MS));

        uint32_t current_time = now_ms();
        bool all_healthy = true;
        health_channel_t failed_ch = HEALTH_CHANNEL_COUNT;
        uint32_t failed_elapsed = 0;

        for (int i = 0; i < HEALTH_CHANNEL_COUNT; i++) {
            if (!s_channels[i].enabled) continue;

            uint32_t last = s_channels[i].last_heartbeat_ms;
            uint32_t elapsed = (last > 0 && current_time >= last) ? (current_time - last) : 0;

            if (elapsed > s_channels[i].max_interval_ms) {
                all_healthy = false;
                failed_ch = (health_channel_t)i;
                failed_elapsed = elapsed;
                break;
            }
        }

        if (all_healthy) {
            // Todos los canales reportaron latido a tiempo -> alimentar el hardware TWDT
            if (s_twdt_user != NULL) {
                esp_task_wdt_reset_user(s_twdt_user);
            }
        } else {
            // UN CANAL CRÍTICO SE COLGÓ
            uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
            s_rtc_data.magic = RTC_HEALTH_MAGIC;
            s_rtc_data.had_crash = true;
            s_rtc_data.reset_count++;
            s_rtc_data.prev_uptime_s = uptime_s;
            s_rtc_data.failing_channel = failed_ch;
            snprintf(s_rtc_data.failing_task, sizeof(s_rtc_data.failing_task), "%s", s_channels[failed_ch].name);
            snprintf(s_rtc_data.reason, sizeof(s_rtc_data.reason),
                     "stalled for %u s (limit %u s)",
                     (unsigned)(failed_elapsed / 1000),
                     (unsigned)(s_channels[failed_ch].max_interval_ms / 1000));

            ESP_LOGE(TAG, "=======================================================");
            ESP_LOGE(TAG, ">>> CRITICAL WATCHDOG STALL: Task '%s' is frozen!", s_channels[failed_ch].name);
            ESP_LOGE(TAG, ">>> Last heartbeat %u s ago (max allowed: %u s). Uptime: %u s",
                     (unsigned)(failed_elapsed / 1000),
                     (unsigned)(s_channels[failed_ch].max_interval_ms / 1000),
                     (unsigned)uptime_s);
            ESP_LOGE(TAG, ">>> Withholding TWDT feed and forcing esp_restart()...");
            ESP_LOGE(TAG, "=======================================================");

            // NO alimentamos TWDT. Damos 500ms para flush de log serial y forzamos restart.
            // Si esp_restart() fallase o se bloqueara, el TWDT de hardware reseteará el chip.
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }
    }
}

esp_err_t system_health_init(void) {
    if (s_initialized) return ESP_OK;

    // 0. Inicializar búfer de logs en vivo en PSRAM y enganchar vprintf
    if (s_live_log_buf == NULL) {
        s_live_log_buf = heap_caps_malloc(LIVE_LOG_BUF_SIZE, MALLOC_CAP_SPIRAM);
        if (s_live_log_buf == NULL) {
            ESP_LOGW(TAG, "PSRAM log buffer alloc failed, using internal SRAM 8KB");
            s_live_log_buf = malloc(8192);
        }
        if (s_live_log_buf != NULL) {
            s_default_vprintf = esp_log_set_vprintf(health_vprintf);
            ESP_LOGI(TAG, "Log capture hook installed (buffer: %s)",
                     esp_psram_is_initialized() ? "64KB PSRAM" : "8KB SRAM");
        }
    }

    // 1. Validar e inspeccionar memoria RTC
    esp_reset_reason_t rst_reason = esp_reset_reason();
    ESP_LOGI(TAG, "esp_reset_reason() = %d", (int)rst_reason);

    if (s_rtc_data.magic != RTC_HEALTH_MAGIC) {
        ESP_LOGI(TAG, "RTC health memory cold boot — initializing structure");
        memset(&s_rtc_data, 0, sizeof(s_rtc_data));
        s_rtc_data.magic = RTC_HEALTH_MAGIC;
    } else {
        if (rst_reason == ESP_RST_TASK_WDT || rst_reason == ESP_RST_WDT || rst_reason == ESP_RST_INT_WDT) {
            s_rtc_data.had_crash = true;
            if (s_rtc_data.reason[0] == 0) {
                snprintf(s_rtc_data.reason, sizeof(s_rtc_data.reason), "hardware watchdog reset (%d)", (int)rst_reason);
            }
        }
        if (s_rtc_data.had_crash) {
            ESP_LOGW(TAG, "PREVIOUS CRASH DETECTED in task '%s': %s (prev uptime: %u s, total recoveries: %u)",
                     s_rtc_data.failing_task[0] ? s_rtc_data.failing_task : "unknown",
                     s_rtc_data.reason,
                     (unsigned)s_rtc_data.prev_uptime_s,
                     (unsigned)s_rtc_data.reset_count);
        }
    }

    // 2. Suscripción al Task Watchdog Timer (TWDT)
    esp_err_t werr = esp_task_wdt_add_user("system_health", &s_twdt_user);
    if (werr != ESP_OK) {
        ESP_LOGW(TAG, "esp_task_wdt_add_user returned %s — trying esp_task_wdt_init", esp_err_to_name(werr));
        esp_task_wdt_config_t twdt_cfg = {
            .timeout_ms = 60000,
            .idle_core_mask = (1 << 0) | (1 << 1),
            .trigger_panic = true,
        };
        esp_task_wdt_init(&twdt_cfg);
        werr = esp_task_wdt_add_user("system_health", &s_twdt_user);
    }

    if (werr == ESP_OK && s_twdt_user != NULL) {
        ESP_LOGI(TAG, "Subscribed to hardware TWDT successfully");
        esp_task_wdt_reset_user(s_twdt_user);
    } else {
        ESP_LOGE(TAG, "Failed to subscribe to hardware TWDT (%s)", esp_err_to_name(werr));
    }

    // 3. Crear tarea supervisora
    BaseType_t ok = xTaskCreate(supervisor_task, "sys_health",
                                SUPERVISOR_TASK_STACK, NULL, SUPERVISOR_TASK_PRIO, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(sys_health) failed");
        return ESP_FAIL;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "System Health Supervisor initialized OK");
    return ESP_OK;
}

void system_health_format_status(char *out, size_t out_size) {
    if (out == NULL || out_size == 0) return;

    uint32_t current_time = now_ms();
    int p = snprintf(out, out_size, "Health Supervisor: OK (recoveries: %u)\n", (unsigned)s_rtc_data.reset_count);

    for (int i = 0; i < HEALTH_CHANNEL_COUNT && p < (int)out_size - 64; i++) {
        if (!s_channels[i].enabled) {
            p += snprintf(out + p, out_size - p, "  [%s]: disabled\n", s_channels[i].name);
            continue;
        }
        uint32_t last = s_channels[i].last_heartbeat_ms;
        uint32_t elapsed = (last > 0 && current_time >= last) ? (current_time - last) : 0;
        bool stalled = (elapsed > s_channels[i].max_interval_ms);

        p += snprintf(out + p, out_size - p, "  [%s]: %s (%u s ago / max %u s)\n",
                      s_channels[i].name,
                      stalled ? "STALLED" : "OK",
                      (unsigned)(elapsed / 1000),
                      (unsigned)(s_channels[i].max_interval_ms / 1000));
    }

    if (s_rtc_data.had_crash && p < (int)out_size - 128) {
        p += snprintf(out + p, out_size - p, "  Last crash: %s in '%s' (prev uptime %u s)\n",
                      s_rtc_data.reason, s_rtc_data.failing_task, (unsigned)s_rtc_data.prev_uptime_s);
    }
    (void)p;
}

bool system_health_check_crash_report(health_crash_report_t *report, bool consume) {
    if (s_rtc_data.magic != RTC_HEALTH_MAGIC || !s_rtc_data.had_crash) {
        return false;
    }
    if (report != NULL) {
        report->had_crash = s_rtc_data.had_crash;
        report->reset_count = s_rtc_data.reset_count;
        report->prev_uptime_s = s_rtc_data.prev_uptime_s;
        strncpy(report->failing_task, s_rtc_data.failing_task, sizeof(report->failing_task) - 1);
        report->failing_task[sizeof(report->failing_task) - 1] = 0;
        strncpy(report->reason, s_rtc_data.reason, sizeof(report->reason) - 1);
        report->reason[sizeof(report->reason) - 1] = 0;
    }
    if (consume) {
        s_rtc_data.had_crash = false;
    }
    return true;
}

void system_health_save_rtc_time(time_t unix_sec) {
    if (unix_sec < 1704067200) return; // Ignorar pre-2024
    s_rtc_data.magic = RTC_HEALTH_MAGIC;
    s_rtc_data.rtc_unix_sec = unix_sec;
    s_rtc_data.rtc_save_esp_timer_us = esp_timer_get_time();
    ESP_LOGD(TAG, "saved RTC time: %ld", (long)unix_sec);
}

bool system_health_restore_rtc_time(time_t *out_unix_sec) {
    if (s_rtc_data.magic != RTC_HEALTH_MAGIC || s_rtc_data.rtc_unix_sec < 1704067200) {
        return false;
    }
    if (out_unix_sec != NULL) {
        *out_unix_sec = s_rtc_data.rtc_unix_sec;
    }
    return true;
}

size_t system_health_get_live_logs(char *out, size_t max_len) {
    if (out == NULL || max_len <= 1) return 0;
    portENTER_CRITICAL(&s_log_mux);
    if (s_live_log_buf == NULL || s_live_log_len == 0) {
        portEXIT_CRITICAL(&s_log_mux);
        out[0] = 0;
        return 0;
    }
    size_t count = s_live_log_len;
    if (count >= max_len) count = max_len - 1;

    size_t cap = (s_live_log_buf != NULL && esp_psram_is_initialized()) ? LIVE_LOG_BUF_SIZE : 8192;
    size_t start = (s_live_log_head + cap - s_live_log_len) % cap;
    if (s_live_log_len > count) {
        start = (start + (s_live_log_len - count)) % cap;
    }

    for (size_t i = 0; i < count; i++) {
        out[i] = s_live_log_buf[(start + i) % cap];
    }
    out[count] = 0;
    portEXIT_CRITICAL(&s_log_mux);
    return count;
}

size_t system_health_get_crash_logs(char *out, size_t max_len) {
    if (out == NULL || max_len <= 1) return 0;
    portENTER_CRITICAL(&s_log_mux);
    if (s_rtc_data.crash_log_len == 0 || s_rtc_data.magic != RTC_HEALTH_MAGIC) {
        portEXIT_CRITICAL(&s_log_mux);
        out[0] = 0;
        return 0;
    }
    size_t count = s_rtc_data.crash_log_len;
    if (count >= max_len) count = max_len - 1;

    size_t start = (s_rtc_data.crash_log_head + RTC_CRASH_LOG_SIZE - s_rtc_data.crash_log_len) % RTC_CRASH_LOG_SIZE;
    if (s_rtc_data.crash_log_len > count) {
        start = (start + (s_rtc_data.crash_log_len - count)) % RTC_CRASH_LOG_SIZE;
    }

    for (size_t i = 0; i < count; i++) {
        out[i] = s_rtc_data.crash_log[(start + i) % RTC_CRASH_LOG_SIZE];
    }
    out[count] = 0;
    portEXIT_CRITICAL(&s_log_mux);
    return count;
}

void system_health_clear_crash_logs(void) {
    portENTER_CRITICAL(&s_log_mux);
    s_rtc_data.crash_log_head = 0;
    s_rtc_data.crash_log_len = 0;
    memset(s_rtc_data.crash_log, 0, sizeof(s_rtc_data.crash_log));
    portEXIT_CRITICAL(&s_log_mux);
}
