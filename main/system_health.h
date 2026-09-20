#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Canales de latido (heartbeat) para las tareas críticas del sistema.
 * Cada tarea debe reportar periódicamente su latido. Si alguna tarea
 * crítica se cuelga (deadlock, socket bloqueado, bucle infinito, etc.),
 * el supervisor no alimenta el Task Watchdog Timer (TWDT) de hardware
 * y fuerza un reinicio seguro del ESP32.
 */
typedef enum {
    HEALTH_CHANNEL_SCHEDULER = 0, // scheduler_task (wake/off diario)
    HEALTH_CHANNEL_AUTOMATION,    // automation_task (inactividad suspend/off)
    HEALTH_CHANNEL_WIFI,          // wifi_keepalive_task (supervisión WiFi)
    HEALTH_CHANNEL_STATE,         // state_poll_task (sondeo cocopa01)
    HEALTH_CHANNEL_TELEGRAM,      // telegram_poll_task (long-poll Telegram)
    HEALTH_CHANNEL_AUTO_REBOOT,   // auto_reboot_task (temporizador de reboot)
    HEALTH_CHANNEL_COUNT
} health_channel_t;

/* Diagnóstico de recuperación post-reinicio almacenado en RTC Fast Memory */
typedef struct {
    bool     had_crash;           // true si el boot actual fue por un stall de tarea o watchdog
    uint32_t reset_count;         // contador acumulado de recuperaciones
    uint32_t prev_uptime_s;       // uptime que tenía el ESP32 antes de caer
    char     failing_task[32];    // nombre de la tarea que falló
    char     reason[64];          // descripción del fallo
} health_crash_report_t;

/* Inicializa el supervisor de salud del sistema, suscribe el usuario al
 * Task Watchdog Timer (TWDT) de ESP-IDF y arranca la tarea de supervisión. */
esp_err_t system_health_init(void);

/* Reporta un latido (heartbeat) para el canal especificado.
 * Seguro desde cualquier tarea FreeRTOS. */
void system_health_heartbeat(health_channel_t channel);

/* Habilita o deshabilita la supervisión de un canal.
 * Útil para tareas que aún no han sido inicializadas. */
void system_health_enable_channel(health_channel_t channel, bool enable);

/* Obtiene el estado actual de todos los canales en formato de texto
 * legible para Telegram (/diag) o consola serial. */
void system_health_format_status(char *out, size_t out_size);

/* Consulta si hubo una recuperación por cuelgue en este boot.
 * Llena report si no es NULL. Si consume es true, limpia el flag
 * para que solo se reporte una vez a Telegram. */
bool system_health_check_crash_report(health_crash_report_t *report, bool consume);

/* Guarda la última hora UTC confirmada por SNTP en memoria RTC. */
void system_health_save_rtc_time(time_t unix_sec);

/* Intenta restaurar la hora UTC desde memoria RTC si SNTP aún no ha sincronizado.
 * Retorna true si la hora en RTC es válida (posterior a 2024). */
bool system_health_restore_rtc_time(time_t *out_unix_sec);

/* Extrae el búfer de logs en vivo (PSRAM) linealizado de más antiguo a más nuevo.
 * Retorna la cantidad de bytes escritos en out (terminado en null). */
size_t system_health_get_live_logs(char *out, size_t max_len);

/* Extrae el búfer de logs del último cuelgue (RTC Memory) linealizado.
 * Retorna la cantidad de bytes escritos en out (terminado en null). */
size_t system_health_get_crash_logs(char *out, size_t max_len);

/* Limpia el búfer de crash logs de memoria RTC. */
void system_health_clear_crash_logs(void);

#ifdef __cplusplus
}
#endif
