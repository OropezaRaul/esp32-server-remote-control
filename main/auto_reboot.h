#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define AUTO_REBOOT_STATUS_BUF_SIZE 384

// Inicializa el temporizador de auto-reboot: carga configuración desde NVS
// (namespace "autoreboot", key "interval_min"), o default de Kconfig (60 min).
// Arranca la tarea en background que supervisa el tiempo transcurrido.
esp_err_t auto_reboot_init(void);

// Retorna el intervalo actual configurado en minutos. 0 = deshabilitado.
uint32_t auto_reboot_get_interval_min(void);

// Configura el intervalo en minutos y lo persiste en NVS.
// 0 = deshabilita el auto-reboot ("switch off").
// Retorna ESP_OK en éxito.
esp_err_t auto_reboot_set_interval_min(uint32_t minutes);

// Retorna true si el auto-reboot periódico está activo (intervalo > 0).
bool auto_reboot_is_enabled(void);

// Retorna los segundos restantes para el próximo auto-reboot,
// o 0 si está deshabilitado o si el tiempo ya expiró.
uint32_t auto_reboot_seconds_remaining(void);

// Llena `out` con un resumen multilínea formateado para Telegram / serial:
// estado (habilitado/deshabilitado), intervalo en minutos, uptime del ESP32,
// y tiempo restante para el próximo reinicio automático.
void auto_reboot_format_status(char *out, size_t out_size);

// Dispara un reinicio del ESP32.
// Si reply_chat_id != 0, envía confirmación directa a ese chat antes del restart.
void auto_reboot_trigger(const char *reason, int64_t reply_chat_id);
