#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>

#include "power_event_logic.h"

static int checks = 0;

static void check(bool cond, const char *what) {
    checks++;
    if (!cond) {
        printf("  FAIL: %s\n", what);
        exit(1);
    }
}

#define MIN 60000UL

int main(void) {
    printf("=== power_event_logic: deteccion de cortes y escalada ===\n");

    // 1. Normal: el servidor esta apagado a proposito y la red se mueve.
    {
        power_event_t e;
        power_event_logic_init(&e, 1000);
        power_event_logic_note_intentional_off(&e, 1000);
        power_event_logic_note_net_down(&e, 2000);
        power_event_logic_note_net_up(&e, 2000 + 120000);
        check(e.phase == POWER_EVENT_PHASE_NORMAL,
              "apagado deliberado + corte -> no se escala");
        check(power_event_logic_decide(&e, 300000, false) == POWER_EVENT_ACTION_NONE,
              "sin corte: ninguna accion");
    }

    // 2. El servidor estaba ON y hay corte largo -> se escala (WOL).
    {
        power_event_t e;
        power_event_logic_init(&e, 0);
        power_event_logic_note_server_on(&e, 1000);
        power_event_logic_note_net_down(&e, 2000);
        power_event_logic_note_net_up(&e, 2000 + 120000);
        check(e.phase == POWER_EVENT_PHASE_WAITING_SERVER,
              "corte con server ON -> esperando al servidor");
        check(power_event_logic_decide(&e, 122000, false) == POWER_EVENT_ACTION_WAKE,
              "primer intento inmediato");
        check(e.attempts == 1, "intentos = 1");
        // El siguiente intento espera 2 min (descartar perdida de red).
        check(power_event_logic_decide(&e, 130000, false) == POWER_EVENT_ACTION_NONE,
              "no reintenta antes de la espera");
        check(power_event_logic_decide(&e, 122000 + 120000, false) == POWER_EVENT_ACTION_WAKE,
              "reintenta tras la espera");
    }

    // 3. Un blip de red (corto) NO es un corte.
    {
        power_event_t e;
        power_event_logic_init(&e, 0);
        power_event_logic_note_server_on(&e, 1000);
        power_event_logic_note_net_down(&e, 2000);
        power_event_logic_note_net_up(&e, 2000 + 5000);
        check(e.phase == POWER_EVENT_PHASE_NORMAL,
              "blip de 5 s -> no es corte");
        check(power_event_logic_decide(&e, 600000, false) == POWER_EVENT_ACTION_NONE,
              "blip: no se escala");
    }

    // 4. El servidor vuelve -> aviso de "volvio" y se limpia el estado.
    {
        power_event_t e;
        power_event_logic_init(&e, 0);
        power_event_logic_note_server_on(&e, 1000);
        power_event_logic_note_net_down(&e, 2000);
        power_event_logic_note_net_up(&e, 2000 + 120000);
        power_event_logic_decide(&e, 122000, false);
        check(power_event_logic_decide(&e, 130000, true) == POWER_EVENT_ACTION_NOTIFY_BACK,
              "el servidor volvio -> aviso");
        check(e.phase == POWER_EVENT_PHASE_NORMAL, "volvio -> fase normal");
        check(power_event_logic_decide(&e, 140000, true) == POWER_EVENT_ACTION_NONE,
              "sin avisos repetidos");
    }

    // 5. Se agotan los intentos -> aviso y se rinde (noinsetento infinitos).
    {
        power_event_t e;
        power_event_logic_init(&e, 0);
        power_event_logic_note_server_on(&e, 1000);
        power_event_logic_note_net_down(&e, 2000);
        power_event_logic_note_net_up(&e, 2000 + 120000);
        uint32_t t = 122000;
        power_event_action_t act = POWER_EVENT_ACTION_NONE;
        int wakes = 0, notifies = 0;
        for (int i = 0; i < 40; i++) {
            act = power_event_logic_decide(&e, t, false);
            if (act == POWER_EVENT_ACTION_WAKE) wakes++;
            if (act == POWER_EVENT_ACTION_NOTIFY) notifies++;
            t += 120000;
        }
        check(wakes == (int)POWER_EVENT_WAKE_MAX_ATTEMPTS,
              "exactamente MAX intentos de WOL");
        check(notifies == 1, "un solo aviso de que no volvio");
        check(e.gave_up, "se rindio");
    }

    // 6. Tras un reinicio del ESP32 en mitad del corte, sigue esperando.
    {
        power_event_t e;
        power_event_logic_init(&e, 0);
        power_event_logic_note_server_on(&e, 1000);
        power_event_logic_note_net_down(&e, 2000);
        power_event_logic_note_net_up(&e, 2000 + 120000);
        power_event_logic_decide(&e, 122000, false);
        // el ESP32 se reinicia: se restaura el estado persistido
        power_event_t r;
        r.phase = e.phase;
        r.server_was_on = e.server_was_on;
        r.notified = e.notified;
        r.gave_up = e.gave_up;
        r.attempts = e.attempts;
        r.outage_ms = e.outage_ms;
        r.next_action_ms = 0;
        power_event_logic_restore(&r, 500000);
        check(r.phase == POWER_EVENT_PHASE_WAITING_SERVER,
              "tras reiniciar sigue esperando al servidor");
        check(power_event_logic_decide(&r, 500000, false) == POWER_EVENT_ACTION_WAKE,
              "y sigue con la escalada");
    }

    // 7. El ESP32 estuvo apagado mucho (hueco de reloj) -> mismo tratamiento
    //    que un corte: a encender el servidor.
    {
        power_event_t e;
        power_event_logic_init(&e, 0);
        power_event_logic_note_server_on(&e, 1000);
        power_event_logic_note_gap(&e, 3UL * 3600UL * 1000UL, 5000);
        check(e.phase == POWER_EVENT_PHASE_WAITING_SERVER,
              "apagon largo -> espera al servidor");
        check(power_event_logic_decide(&e, 5000, false) == POWER_EVENT_ACTION_WAKE,
              "apagon largo -> primer intento");
    }

    // 8. Un hueco con el servidor apagado a proposito no escala nada.
    {
        power_event_t e;
        power_event_logic_init(&e, 0);
        power_event_logic_note_intentional_off(&e, 1000);
        power_event_logic_note_gap(&e, 3UL * 3600UL * 1000UL, 5000);
        check(e.phase == POWER_EVENT_PHASE_NORMAL,
              "apagon largo con server apagado a proposito -> nada");
    }

    // 9. Agotados los intentos se olvida que el servidor debia estar encendido
    //    (puede que el usuario lo apagara a mano): no se repite la escalada.
    {
        power_event_t e;
        power_event_logic_init(&e, 0);
        power_event_logic_note_server_on(&e, 1000);
        power_event_logic_note_net_down(&e, 2000);
        power_event_logic_note_net_up(&e, 2000 + 120000);
        uint32_t t2 = 122000;
        power_event_action_t a2 = POWER_EVENT_ACTION_NONE;
        for (int i = 0; i < 40; i++) {
            a2 = power_event_logic_decide(&e, t2, false);
            t2 += 120000;
        }
        check(a2 == POWER_EVENT_ACTION_NONE, "tras rendirse: ninguna accion mas");
        check(!e.server_was_on, "tras rendirse forgets que debia estar encendido");
    }

    printf("  OK: %d checks\n", checks);
    return 0;
}
