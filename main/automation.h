#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Configuración de automatización basada en inactividad.
//
// Dos temporizadores consecutivos, ambos reiniciados por `automation_kick()`:
//   1) suspend_after_min  minutos de inactividad  -> POST /suspend  (S3 RAM)
//   2) off_after_min      minutos desde la suspensión (no desde el kick)
//                          -> POST /off (S5) si NO estamos en horario protegido
//
// Horario protegido (configurable en minutos desde medianoche, hora local):
//   [quiet_start_min, quiet_end_min)
//   Wrap-around soportado: si quiet_end_min < quiet_start_min, el rango cruza
//   medianoche. Si ambos son iguales, no hay horario protegido.
//
// Toda la configuración se persiste en NVS bajo el namespace "automation".
// Los defaults vienen de Kconfig (main/Kconfig.projbuild) pero pueden
// sobreescribirse en runtime con /autoset sin reflashear.

typedef struct {
    bool     enabled;
    uint16_t suspend_after_min;   // 0 deshabilita auto-suspend
    uint16_t off_after_min;       // 0 deshabilita auto-off (solo suspende)
    uint16_t quiet_start_min;     // 0..1439 (ej. 8:00  = 480)
    uint16_t quiet_end_min;       // 0..1439 (ej. 20:00 = 1200)
} automation_config_t;

// Tamaño máximo del string que retorna automation_status_str().
#define AUTOMATION_STATUS_BUF_SIZE 512

// Inicializa el módulo: carga config desde NVS (o defaults si no existe),
// arranca la tarea de supervisión. Llamar después de state_init() y
// server_client disponible.
esp_err_t automation_init(void);

// Llamar desde telegram.c, web_server.c y state.c ante cualquier evento
// que indique que el usuario está interactuando con el sistema. Es seguro
// llamarlo antes de automation_init() (no-op si g_initialized == false).
void automation_kick(void);

// Reemplaza la configuración actual y persiste en NVS. Apunta `cfg` puede
// ser NULL para resetear a defaults de Kconfig.
esp_err_t automation_set_config(const automation_config_t *cfg);

// Carga la configuración actualmente en uso (defaults si NVS está vacío).
void automation_get_config(automation_config_t *out);

// Resetea la configuración a los defaults de Kconfig y persiste en NVS.
esp_err_t automation_reset_defaults(void);

// Parsea una cadena "HH:MM" (acepta también "H:MM", "HH:MM-HH:MM" solo el
// primer HH:MM). Devuelve minutos desde medianoche o -1 si inválido.
int automation_parse_hhmm(const char *s);

// Llena `out` con un texto multi-línea listo para enviar por Telegram con
// el estado actual de la automatización (config + última interacción +
// tiempo restante estimado). `out` debe apuntar a un buffer de al menos
// AUTOMATION_STATUS_BUF_SIZE bytes.
void automation_status_str(char *out, size_t out_size);

// Devuelve true si la automatización está habilitada (atajo).
bool automation_is_enabled(void);
