#pragma once

#include <stdbool.h>

// Decision (pura, testeable en host) de como apagar server.
//
// El bug que motivo este modulo (2026-10-20): cuando server no respondia,
// el /off de Telegram contestaba "OK: server not reachable; assuming off."
// SIN enviar nada. El servidor estaba en S3 (suspendido, con su Tailscale
// caida) y se quedaba encendido para siempre.
//
// La regla que lo evita:
//   * si el servidor responde  -> /off directo.
//   * si NO responde pero hay RASTRO de suspension -> hay que despertarlo con
//     WOL primero (ACPI no permite S3 -> S5 directo) y despues mandarle /off.
//   * si NO responde y NO hay rastro -> esta apagado de verdad. NO se manda un
//     magic packet: eso encenderia el equipo (y con el, todo el stack).

typedef enum {
    POWER_OFF_DECISION_SEND_OFF = 0,   // manda /off ahora
    POWER_OFF_DECISION_WAKE_THEN_OFF,   // despierta (WOL) y despues /off
    POWER_OFF_DECISION_ALREADY_OFF,     // no hay nada que apagar
} power_off_decision_t;

power_off_decision_t power_off_decide(bool tcp_alive, bool suspect_suspended);
