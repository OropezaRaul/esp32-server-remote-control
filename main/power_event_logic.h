#pragma once

#include <stdbool.h>
#include <stdint.h>

// Nucleo PURO de la deteccion de cortes y la escalada de encendido.
// Sin ESP-IDF ni FreeRTOS: se compila y testea en host (tools/tests/power_event_test.c).
// Mismo patron que wake_logic.h/wake_logic.c.

// De donde sale la idea: el ESP32 y el servidor estan en la misma instalacion
// (mismo router, mismo circuito). Si se corta la luz, el ESP32 tambien pierde
// el WiFi; cuando vuelve la red, el servidor deberia haber vuelto con ella. Si el
// servidor estaba ENCENDIDO y no vuelve, casi seguro es un corte (no un apagado
// deliberado).
//
// Eso permite dos cosas que antes no existian:
//   * intentar el encendido SOLO cuando hay un corte probable (asi no se
//     enciende un servidor que el usuario apagó a mano), y
//   * avisar por Telegram cuando el servidor no vuelve, que es el dato que
//     hace falta para ir a apretar el boton.

typedef enum {
    POWER_EVENT_PHASE_NORMAL = 0,       // nada especial
    POWER_EVENT_PHASE_NET_DOWN,          // el ESP32 perdio la red (posible corte)
    POWER_EVENT_PHASE_WAITING_SERVER,    // la red volvio; el servidor sigue caido
} power_event_phase_t;

typedef enum {
    POWER_EVENT_ACTION_NONE = 0,         // no hacer nada
    POWER_EVENT_ACTION_WAKE,             // armar/rearmar un intento de WOL
    POWER_EVENT_ACTION_NOTIFY,           // avisar: el servidor no volvio
    POWER_EVENT_ACTION_NOTIFY_BACK,      // avisar: el servidor volvio
} power_event_action_t;

typedef struct {
    power_event_phase_t phase;
    bool     server_was_on;   // el servidor estaba encendido cuando se perdio
    bool     notified;        // ya se aviso que no volvio
    bool     gave_up;         // se agotaron los intentos (no reintentar mas)
    uint32_t outage_ms;       // duracion de la caida de red
    uint32_t attempts;        // intentos de despertar
    uint32_t next_action_ms;  // proximo momento de actuar
} power_event_t;

// Una caida de red de menos de esto NO es un corte (un blip del AP no cuenta).
#define POWER_EVENT_MIN_OUTAGE_MS      20000UL
// Espera entre intentos de despertar: lo suficiente para que el router, el AP y
// la NIC del servidor se re-estabilicen tras volver la luz.
#define POWER_EVENT_WAKE_RETRY_MS      120000UL
// Maximo de intentos antes de rendirse y avisar (15 x 2 min = ~30 min).
#define POWER_EVENT_WAKE_MAX_ATTEMPTS  15U

void power_event_logic_init(power_event_t *e, uint32_t now_ms);
// El ESP32 perdio la red (STA_DISCONNECTED).
void power_event_logic_note_net_down(power_event_t *e, uint32_t now_ms);
// El ESP32 recupero la red (GOT_IP).
void power_event_logic_note_net_up(power_event_t *e, uint32_t now_ms);
// El servidor respondio (volvio).
void power_event_logic_note_server_on(power_event_t *e, uint32_t now_ms);
// Nosotros apagamos el servidor a proposito: no hay nada que recuperar.
void power_event_logic_note_intentional_off(power_event_t *e, uint32_t now_ms);
// Tras un reinicio del ESP32: si estabamos esperando al servidor, seguimos esperando.
void power_event_logic_restore(power_event_t *e, uint32_t now_ms);
// El ESP32 estuvo apagado gap_ms (tiempo de pared) entre dos arranques: si
// fue mucho mas que su propio intervalo de auto-reboot, se apago por un corte
// (no por su temporizador) -> mismo tratamiento que un corte de red.
void power_event_logic_note_gap(power_event_t *e, uint32_t gap_ms, uint32_t now_ms);

// Que hay que hacer ahora. server_alive = el servidor responde.
power_event_action_t power_event_logic_decide(power_event_t *e, uint32_t now_ms,
                                              bool server_alive);
