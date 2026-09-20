#include "auto_reboot.h"

#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>

#include "telegram.h"
#include "system_health.h"
#include "sdkconfig.h"

static const char *TAG = "auto_reboot";

#define NVS_NAMESPACE    "autoreboot"
#define NVS_KEY_INTERVAL "interval_min"

#ifdef CONFIG_AUTO_REBOOT_INTERVAL_MIN
#define DEFAULT_INTERVAL_MIN CONFIG_AUTO_REBOOT_INTERVAL_MIN
#else
#define DEFAULT_INTERVAL_MIN 60
#endif

#define TASK_STACK_SIZE  4096
#define TASK_PRIORITY    3

static uint32_t s_interval_min = DEFAULT_INTERVAL_MIN;
static int64_t s_boot_time_us = 0;
static int64_t s_timer_base_us = 0;
static bool s_initialized = false;
static TaskHandle_t s_task_handle = NULL;

void auto_reboot_trigger(const char *reason, int64_t reply_chat_id) {
    ESP_LOGW(TAG, ">>> Rebooting ESP32: %s", reason ? reason : "unspecified");

    // Persistir el offset de Telegram procesado para no repetir /reboot
    telegram_save_last_uid();

    if (reply_chat_id != 0) {
        char msg[96];
        uint32_t interval = s_interval_min;
        if (interval > 0) {
            snprintf(msg, sizeof(msg), "OK: Reiniciando ESP32... (Auto-reboot: cada %lu min)",
                     (unsigned long)interval);
        } else {
            snprintf(msg, sizeof(msg), "OK: Reiniciando ESP32... (Auto-reboot: desactivado)");
        }
        telegram_send_text(reply_chat_id, msg);
    }

    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
}

static esp_err_t load_from_nvs(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no NVS config — using default (%u min)", (unsigned)DEFAULT_INTERVAL_MIN);
        s_interval_min = DEFAULT_INTERVAL_MIN;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open read failed: %s", esp_err_to_name(err));
        s_interval_min = DEFAULT_INTERVAL_MIN;
        return err;
    }

    uint32_t u32 = 0;
    err = nvs_get_u32(h, NVS_KEY_INTERVAL, &u32);
    if (err == ESP_OK) {
        s_interval_min = u32;
        ESP_LOGI(TAG, "loaded interval from NVS: %lu min", (unsigned long)s_interval_min);
    } else {
        s_interval_min = DEFAULT_INTERVAL_MIN;
        ESP_LOGI(TAG, "no interval in NVS — using default (%u min)", (unsigned)DEFAULT_INTERVAL_MIN);
    }
    nvs_close(h);
    return ESP_OK;
}

static esp_err_t save_to_nvs(uint32_t interval) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open write failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_u32(h, NVS_KEY_INTERVAL, interval);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to persist interval to NVS: %s", esp_err_to_name(err));
    }
    return err;
}

uint32_t auto_reboot_get_interval_min(void) {
    return s_interval_min;
}

esp_err_t auto_reboot_set_interval_min(uint32_t minutes) {
    esp_err_t err = save_to_nvs(minutes);
    if (err != ESP_OK) {
        return err;
    }
    s_interval_min = minutes;
    s_timer_base_us = esp_timer_get_time();
    ESP_LOGI(TAG, "auto-reboot interval updated to %lu min", (unsigned long)minutes);
    return ESP_OK;
}

bool auto_reboot_is_enabled(void) {
    return (s_interval_min > 0);
}

uint32_t auto_reboot_seconds_remaining(void) {
    if (s_interval_min == 0) return 0;

    int64_t now_us = esp_timer_get_time();
    int64_t elapsed_us = now_us - s_timer_base_us;
    int64_t target_us = (int64_t)s_interval_min * 60LL * 1000000LL;

    if (elapsed_us >= target_us) return 0;
    return (uint32_t)((target_us - elapsed_us) / 1000000LL);
}

void auto_reboot_format_status(char *out, size_t out_size) {
    if (out == NULL || out_size == 0) return;

    uint32_t interval = s_interval_min;
    int64_t now_us = esp_timer_get_time();
    uint32_t uptime_s = (uint32_t)(now_us / 1000000ULL);
    uint32_t up_h = uptime_s / 3600;
    uint32_t up_m = (uptime_s % 3600) / 60;
    uint32_t up_s = uptime_s % 60;

    if (interval == 0) {
        snprintf(out, out_size,
                 "Auto-reboot: DISABLED (switch off)\n"
                 "ESP32 Uptime: %luh %lum %lus\n"
                 "Use '/reboot <min>' to enable (e.g. /reboot 60).",
                 (unsigned long)up_h, (unsigned long)up_m, (unsigned long)up_s);
    } else {
        uint32_t rem_s = auto_reboot_seconds_remaining();
        uint32_t rem_m = rem_s / 60;
        uint32_t rem_sec = rem_s % 60;
        snprintf(out, out_size,
                 "Auto-reboot: ENABLED\n"
                 "Interval: %lu min (%luh %lum)\n"
                 "ESP32 Uptime: %luh %lum %lus\n"
                 "Next reboot in: %lu min %lu s",
                 (unsigned long)interval,
                 (unsigned long)(interval / 60), (unsigned long)(interval % 60),
                 (unsigned long)up_h, (unsigned long)up_m, (unsigned long)up_s,
                 (unsigned long)rem_m, (unsigned long)rem_sec);
    }
}

static void auto_reboot_task(void *arg) {
    ESP_LOGI(TAG, "auto-reboot task started (interval=%lu min)", (unsigned long)s_interval_min);

    while (1) {
        system_health_heartbeat(HEALTH_CHANNEL_AUTO_REBOOT);
        vTaskDelay(pdMS_TO_TICKS(5000));
        system_health_heartbeat(HEALTH_CHANNEL_AUTO_REBOOT);

        if (s_interval_min == 0) {
            continue;
        }

        int64_t now_us = esp_timer_get_time();
        int64_t elapsed_us = now_us - s_timer_base_us;
        int64_t target_us = (int64_t)s_interval_min * 60LL * 1000000LL;

        if (elapsed_us >= target_us) {
            ESP_LOGW(TAG, "Auto-reboot timer expired (%lu min). Restarting ESP32...",
                     (unsigned long)s_interval_min);
            auto_reboot_trigger("periodic auto-reboot", 0);
        }
    }
}

esp_err_t auto_reboot_init(void) {
    if (s_initialized) return ESP_OK;

    s_boot_time_us = esp_timer_get_time();
    s_timer_base_us = s_boot_time_us;

    load_from_nvs();

    BaseType_t ret = xTaskCreate(auto_reboot_task, "auto_reboot", TASK_STACK_SIZE, NULL, TASK_PRIORITY, &s_task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "failed to create auto_reboot task");
        return ESP_FAIL;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "auto_reboot initialized successfully");
    return ESP_OK;
}
