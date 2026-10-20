#include "power_off_logic.h"

power_off_decision_t power_off_decide(bool tcp_alive, bool suspect_suspended) {
    if (tcp_alive) return POWER_OFF_DECISION_SEND_OFF;
    // No responde: la unica forma de saber si esta en S3 (y hay que
    // despertarlo) o apagado de verdad (y no hay que tocarlo) es el rastro
    // de suspension que lleva el estado.
    if (suspect_suspended) return POWER_OFF_DECISION_WAKE_THEN_OFF;
    return POWER_OFF_DECISION_ALREADY_OFF;
}
