#include "power_event_logic.h"

#include <string.h>

void power_event_logic_init(power_event_t *e, uint32_t now_ms) {
    if (e == NULL) return;
    memset(e, 0, sizeof(*e));
    e->phase = POWER_EVENT_PHASE_NORMAL;
    e->next_action_ms = now_ms;
}

void power_event_logic_note_net_down(power_event_t *e, uint32_t now_ms) {
    if (e == NULL) return;
    if (e->phase == POWER_EVENT_PHASE_NET_DOWN) return;
    e->phase = POWER_EVENT_PHASE_NET_DOWN;
    e->outage_ms = now_ms;      // inicio de la caida
}

void power_event_logic_note_net_up(power_event_t *e, uint32_t now_ms) {
    if (e == NULL) return;
    if (e->phase != POWER_EVENT_PHASE_NET_DOWN) return;

    uint32_t dur = now_ms - e->outage_ms;
    e->outage_ms = dur;
    if (dur < POWER_EVENT_MIN_OUTAGE_MS) {
        // Blip del AP: no es un corte. Se descarta.
        e->phase = POWER_EVENT_PHASE_NORMAL;
        return;
    }
    if (!e->server_was_on) {
        // El servidor estaba apagado a proposito: no hay nada que encender.
        e->phase = POWER_EVENT_PHASE_NORMAL;
        return;
    }
    // La red volvio y el servidor deberia haber vuelto con ella: a esperar.
    e->phase = POWER_EVENT_PHASE_WAITING_SERVER;
    e->attempts = 0;
    e->notified = false;
    e->gave_up = false;
    e->next_action_ms = now_ms;      // primer intento inmediato
}

void power_event_logic_note_server_on(power_event_t *e, uint32_t now_ms) {
    if (e == NULL) return;
    (void)now_ms;
    e->server_was_on = true;
}

void power_event_logic_note_intentional_off(power_event_t *e, uint32_t now_ms) {
    if (e == NULL) return;
    (void)now_ms;
    e->server_was_on = false;
    e->phase = POWER_EVENT_PHASE_NORMAL;
    e->attempts = 0;
    e->notified = false;
    e->gave_up = false;
}

void power_event_logic_restore(power_event_t *e, uint32_t now_ms) {
    if (e == NULL) return;
    if (e->phase == POWER_EVENT_PHASE_NET_DOWN ||
        e->phase == POWER_EVENT_PHASE_WAITING_SERVER) {
        // El ESP32 se reinicio durante el corte: si el servidor deberia estar
        // encendido, seguimos esperando a que vuelva.
        if (e->server_was_on) {
            e->phase = POWER_EVENT_PHASE_WAITING_SERVER;
            e->next_action_ms = now_ms;
        } else {
            e->phase = POWER_EVENT_PHASE_NORMAL;
        }
    }
}

void power_event_logic_note_gap(power_event_t *e, uint32_t gap_ms, uint32_t now_ms) {
    if (e == NULL) return;
    if (e->phase == POWER_EVENT_PHASE_WAITING_SERVER) return;  // ya esperando
    if (!e->server_was_on) return;   // el servidor estaba apagado a proposito
    if (e->phase == POWER_EVENT_PHASE_NET_DOWN) return;        // ya hay corte en curso

    // El ESP32 se reinicio tras un apagon largo: se trata como corte.
    e->phase = POWER_EVENT_PHASE_WAITING_SERVER;
    e->outage_ms = gap_ms;
    e->attempts = 0;
    e->notified = false;
    e->gave_up = false;
    e->next_action_ms = now_ms;
}

power_event_action_t power_event_logic_decide(power_event_t *e, uint32_t now_ms,

                                              bool server_alive) {
    if (e == NULL) return POWER_EVENT_ACTION_NONE;

    if (server_alive) {
        if (e->phase == POWER_EVENT_PHASE_WAITING_SERVER) {
            // Volvio despues del corte (o del apagado por politica).
            e->phase = POWER_EVENT_PHASE_NORMAL;
            e->attempts = 0;
            e->notified = false;
            e->gave_up = false;
            e->server_was_on = true;
            return POWER_EVENT_ACTION_NOTIFY_BACK;
        }
        return POWER_EVENT_ACTION_NONE;
    }

    if (e->phase != POWER_EVENT_PHASE_WAITING_SERVER) return POWER_EVENT_ACTION_NONE;
    if (e->gave_up) return POWER_EVENT_ACTION_NONE;   // ya avisamos y nos rendimos
    if ((int32_t)(now_ms - e->next_action_ms) < 0) return POWER_EVENT_ACTION_NONE;

    if (e->attempts >= POWER_EVENT_WAKE_MAX_ATTEMPTS) {
        e->gave_up = true;
        if (!e->notified) {
            e->notified = true;
            // Nos rendimos: se nos olvida que el servidor "debia" estar
            // encendido. Si en realidad lo apago el usuario a mano, asi no
            // repetimos la escalada en cada corte posterior (y no le
            // encendemos el equipo sin querer).
            e->server_was_on = false;
            return POWER_EVENT_ACTION_NOTIFY;
        }
        return POWER_EVENT_ACTION_NONE;
    }

    e->attempts++;
    e->next_action_ms = now_ms + POWER_EVENT_WAKE_RETRY_MS;
    return POWER_EVENT_ACTION_WAKE;
}
