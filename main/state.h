#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    COCOPA01_STATE_UNKNOWN = 0,
    COCOPA01_STATE_OFF     = 1,
    COCOPA01_STATE_SUSPEND = 2,
    COCOPA01_STATE_ON      = 3,
} cocopa01_state_t;

const char *cocopa01_state_str(cocopa01_state_t s);

void state_init(void);
void state_poll_task(void *arg);

cocopa01_state_t state_get(void);
uint32_t state_entered_at_ms(void);
uint32_t state_last_known_on_ms(void);
uint32_t state_cocopa01_boot_time(void);
uint32_t state_cocopa01_uptime_s(void);
// Momento (ms desde boot) del último poll completado; 0 = nunca polleado.
// Útil para que /status muestre edad real del cache y para diagnosticar
// bloqueos del poll (sin esto el usuario no puede distinguir "OFF hace
// 20 min" de "ESP32 no puede hablar con cocopa01 hace 20 min").
uint32_t state_last_poll_ms(void);
// true si `cocopa01_tcp_alive()` fue exitoso en el último poll (no
// implica certeza sobre el estado real, solo que :22 respondió).
bool state_last_poll_tcp_alive(void);

void state_set(cocopa01_state_t s);
void state_mark_known_on(void);