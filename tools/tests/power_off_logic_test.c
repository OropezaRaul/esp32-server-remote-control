#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>

#include "power_off_logic.h"

static int checks = 0;

static void check(bool cond, const char *what) {
    checks++;
    if (!cond) {
        printf("  FAIL: %s\n", what);
        exit(1);
    }
}

int main(void) {
    printf("=== power_off_logic: decision de apagado de server ===\n");

    // 1. El servidor responde -> /off directo (no hay nada que despertar).
    {
        check(power_off_decide(true, false) == POWER_OFF_DECISION_SEND_OFF,
              "vivo + sin rastro -> SEND_OFF");
        check(power_off_decide(true, true) == POWER_OFF_DECISION_SEND_OFF,
              "vivo + con rastro -> SEND_OFF (responde, no hay que despertar)");
    }

    // 2. No responde CON rastro de suspension -> hay que despertarlo primero.
    //    Este es el caso que rompia: el servidor estaba en S3 (Tailscale
    //    caida) y el /off contestaba "already off" sin apagar nada.
    {
        check(power_off_decide(false, true) == POWER_OFF_DECISION_WAKE_THEN_OFF,
              "muerto + con rastro -> WAKE_THEN_OFF");
    }

    // 3. No responde SIN rastro -> esta apagado de verdad. No se manda magic
    //    packet: despertaria un equipo apagado (y su stack de Docker).
    {
        check(power_off_decide(false, false) == POWER_OFF_DECISION_ALREADY_OFF,
              "muerto + sin rastro -> ALREADY_OFF");
    }

    printf("  OK: %d checks\n", checks);
    return 0;
}
