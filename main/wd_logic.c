#include "wd_logic.h"
#include <stddef.h>

/* Minima staleness del control plane para reportar "zombie" en /diag.
 * En sesion sana el servidor responde PONG a los H2 PING cada ~5s. */
#define WD_ZOMBIE_REPORT_MS  60000u

wd_action_t wd_evaluate(wd_input_t *in) {
    if (in == NULL) return WD_ACT_NONE;
    uint32_t tick = in->tick;

    /* Salidas por defecto. */
    in->disconnect_ms = 0;
    in->zombie = false;

    /* El watchdog solo actua despues de haber visto CONNECTED al menos una
     * vez: evita reboots espurios durante el bootstrap inicial. */
    if (!in->ever_connected) return WD_ACT_NONE;

    /* --- Tiempo sin evidencia REAL de vida de la tailnet --- */
    if (in->state_connected) {
        if (in->ctrl_stale_ms != 0) {
            /* CONNECTED pero silencio del control plane: puede ser una
             * sesion zombie (TCP half-open: los H2 PING se siguen mandando
             * al buffer local, nada vuelve). La staleness de RX es la
             * unica evidencia util. */
            in->disconnect_ms = in->ctrl_stale_ms;
            in->zombie = (in->ctrl_stale_ms >= WD_ZOMBIE_REPORT_MS);
        } else {
            /* CONNECTED y sin historial de RX (recien conectado): sano. */
            in->disconnect_ms = 0;
        }
    } else {
        /* Fuera de CONNECTED (RECONNECTING/WIFI_WAIT/...): se mide desde
         * el ultimo CONNECTED. La resta sin signo de uint32 maneja el wrap
         * del tick (~49.7 dias) de forma natural: NO hay que "corregir"
         * tick < anchor — esa guardia destruiria el calculo al cruzar el
         * wrap (el anchor queda numericamente "en el futuro"). */
        uint32_t anchor = in->last_connected_ms;
        if (anchor == 0) anchor = tick;   /* sin referencia: recien arranco */
        in->disconnect_ms = tick - anchor;
    }

    /* --- Antirebote de REBOOT --- */
    if (in->last_reboot_ms != 0 &&
        (tick - in->last_reboot_ms) < in->cooldown_ms) {
        return WD_ACT_NONE;
    }

    /* --- Escalera de recuperacion --- */
    uint32_t reboot_ms = in->reboot_after_ms;
    uint32_t rebind_ms = in->rebind_after_ms;
    if (rebind_ms >= reboot_ms) rebind_ms = reboot_ms - 1u; /* clamp */

    if (in->disconnect_ms >= reboot_ms) return WD_ACT_REBOOT;
    if (in->disconnect_ms >= rebind_ms && !in->rebind_done) return WD_ACT_REBIND;
    if (in->disconnect_ms >= (reboot_ms / 2u)) return WD_ACT_PRE_WARN;
    return WD_ACT_NONE;
}
