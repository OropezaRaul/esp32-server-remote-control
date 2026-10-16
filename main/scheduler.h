#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Scheduler de encendido/apagado diario a HORARIOS FIJOS (hora Perú, UTC-5).
//
// Es independiente de la automatización por inactividad (/autoset en
// automation.c): dispara a horas fijas todos los días sin importar la
// actividad ni el estado previo. Decisión de diseño del usuario (STATUS.md
// bug #4): el schedule es SIEMPRE la fuente de verdad, no hay manual
// override — si server se apagó a mano a las 23:30, el wake de las 8:00
// dispara igual.
//
//   wake HH:MM -> envía magic packet WOL a server (si ya está ON, skip)
//   off  HH:MM -> POST /off; si está en S3 hace el baile WOL -> S0 -> S5,
//                 igual que el /off manual
//
// Config persistida en NVS namespace "schedule" (sobrevive reboots).
// Sin horarios configurados el módulo no hace nada (estado default).
//
// Consume una tarea propia (stack 8KB, prio 3) que chequea cada 30s.
// Depende de SNTP: automation_init() ya lo inicia; si la hora no está
// sincronizada no dispara nada (y lo reporta en scheduler_status_str).

#define SCHEDULER_STATUS_BUF_SIZE 256

typedef struct {
    bool     wake_set;   // false = sin encendido programado
    uint16_t wake_min;   // 0..1439, minutos desde medianoche (8:00 = 480)
    bool     off_set;    // false = sin apagado programado
    uint16_t off_min;    // 0..1439
} scheduler_config_t;

// Carga la config desde NVS y arranca la tarea de chequeo. Llamar después
// de automation_init() (que inicia SNTP) — no es requisito duro, pero el
// scheduler no dispara nada hasta que la hora esté sincronizada.
esp_err_t scheduler_init(void);

// Reemplaza la config y persiste en NVS. Ambos flags en false equivale a
// reset (los horarios quedan deshabilitados).
esp_err_t scheduler_set(const scheduler_config_t *cfg);

// Carga la config actualmente en uso (sin horarios si NVS está vacío).
void scheduler_get(scheduler_config_t *out);

// Deshabilita wake+off y persiste. Equivale a scheduler_set con ambos unset.
esp_err_t scheduler_reset(void);

// Deshabilita solo el encendido programado (wake) conservando off si existe.
esp_err_t scheduler_clear_wake(void);

// Texto multi-línea listo para Telegram: horarios configurados (o "none"),
// en formato 24h hora Perú, y si SNTP está sincronizado.
void scheduler_status_str(char *out, size_t out_size);

// Marca que la NVS entera fue borrada en este boot (erase-on-error de
// main.c). Llamado una sola vez desde main.c cuando se dispara el wipe.
// El scheduler expone esto porque /schedule es la pieza más visible del
// usuario — si ve "wake: none" sin haberlo reseteado a mano, sabe que
// fue wipe. Ver scheduler_nvs_was_wiped() y scheduler_consume_nvs_wiped().
void scheduler_mark_nvs_wiped(void);

// true si main.c llamó a scheduler_mark_nvs_wiped() en este boot (o si
// la NVS estaba vacía al cargar). Idempotente — el caller decide cuándo
// consumirlo con scheduler_consume_nvs_wiped().
bool scheduler_nvs_was_wiped(void);

// Versión "consume": retorna true la primera vez que se llama después de
// un wipe, false en llamadas subsiguientes. Útil para que telegram_poll_task
// envíe un único aviso por boot.
bool scheduler_consume_nvs_wiped(void);
