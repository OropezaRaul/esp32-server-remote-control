#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    SERVER_STATE_UNKNOWN = 0,
    SERVER_STATE_OFF     = 1,
    SERVER_STATE_SUSPEND = 2,
    SERVER_STATE_ON      = 3,
} server_state_t;

const char *server_state_str(server_state_t s);

void state_init(void);
void state_poll_task(void *arg);

server_state_t state_get(void);
uint32_t state_entered_at_ms(void);
uint32_t state_last_known_on_ms(void);
uint32_t state_server_boot_time(void);
uint32_t state_server_uptime_s(void);
// Momento (ms desde boot) del último poll completado; 0 = nunca polleado.
// Útil para que /status muestre edad real del cache y para diagnosticar
// bloqueos del poll (sin esto el usuario no puede distinguir "OFF hace
// 20 min" de "ESP32 no puede hablar con server hace 20 min").
uint32_t state_last_poll_ms(void);
// true si `server_tcp_alive()` fue exitoso en el último poll (no
// implica certeza sobre el estado real, solo que :22 respondió).
bool state_last_poll_tcp_alive(void);

// true cuando el stack publicable de server (nginx :8080 + tailscaled con
// su Funnel en el nodo) puede atender ya una redireccion del cliente.
//
// Por que existe: medido en campo el 2026-10-15, tras un arranque en frio el
// ESP32 daba server_ready=true a los ~37 s del WOL, pero el tunnel Funnel de
// server recien servia HTTP 200 a los ~46 s. Ese hueco de ~20 s mandaba al
// cliente a un 502 Bad Gateway de Tailscale y se quedaba ahi, sin volver a
// tocar el ESP32. La reanudacion desde S3 NO tiene ese problema (el stack
// sigue en pie: ~5 s), asi que la espera extra solo aplica cuando se detecto
// un boot NUEVO de server desde el ultimo OFF.
bool state_stack_settled(uint32_t cold_boot_min_ms);

// true cuando server NUNCA se apago desde el ultimo OFF: no hubo arranque en
// frio, el stack sigue en pie y no hay nada que esperar. Es la via rapida (sin
// esperas) del resume desde S3, separada de state_stack_settled() porque esta
// ultima exige un minimo de espera y por tanto siempre es falsa durante un
// arranque en frio.
bool state_stack_never_stopped(void);

// true cuando el boot_time guardado proviene de una lectura EN VIVO de /state
// hecha en este boot del ESP32 (no del valor cacheado en NVS al arrancar).
bool state_boot_verified_live(void);

// Contadores de la lectura en vivo de /state (diagnostico via /diag).
uint32_t state_http_stats(uint32_t *attempts, uint32_t *ok, uint32_t *err, int32_t *last_len);

void state_set(server_state_t s);
void state_mark_known_on(void);
// Activa temporalmente el modo de sondeo rápido (cada 2.5s en vez de 30s)
// tras disparar un encendido (WOL) para acelerar la redirección en cuanto
// el servidor esté listo.
void state_trigger_fast_poll(void);
bool state_is_fast_polling(void);