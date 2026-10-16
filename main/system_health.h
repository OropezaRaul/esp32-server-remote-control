#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif







typedef enum {
    HEALTH_CHANNEL_SCHEDULER = 0, // scheduler_task (wake/off diario)
    HEALTH_CHANNEL_AUTOMATION,    // automation_task (inactividad suspend/off)
    HEALTH_CHANNEL_WIFI,          // wifi_keepalive_task (supervisión WiFi)
    HEALTH_CHANNEL_STATE,         // state_poll_task (sondeo cocopa01)
    HEALTH_CHANNEL_TELEGRAM,      // telegram_poll_task (long-poll Telegram)
    HEALTH_CHANNEL_AUTO_REBOOT,   // auto_reboot_task (temporizador de reboot)
    HEALTH_CHANNEL_COUNT
} health_channel_t;


typedef struct {
    bool     had_crash;           // true si el boot actual fue por un stall de tarea o watchdog
    uint32_t reset_count;         // contador acumulado de recuperaciones
    uint32_t prev_uptime_s;       // uptime que tenía el ESP32 antes de caer
    char     failing_task[32];    // nombre de la tarea que falló
    char     reason[64];          // descripción del fallo
} health_crash_report_t;



esp_err_t system_health_init(void);



void system_health_heartbeat(health_channel_t channel);



void system_health_enable_channel(health_channel_t channel, bool enable);



void system_health_format_status(char *out, size_t out_size);




bool system_health_check_crash_report(health_crash_report_t *report, bool consume);


void system_health_save_rtc_time(time_t unix_sec);



bool system_health_restore_rtc_time(time_t *out_unix_sec);



size_t system_health_get_live_logs(char *out, size_t max_len);



size_t system_health_get_crash_logs(char *out, size_t max_len);


void system_health_clear_crash_logs(void);

#ifdef __cplusplus
}
#endif
