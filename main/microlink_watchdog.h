#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Tailscale health watchdog.
 *
 * Tarea periodica que vigila la sesion MicroLink y la recupera con una
 * ESCALERA de acciones:
 *
 *   1. pre-warn (~50% del umbral)  -> solo log.
 *   2. rebind suave                -> microlink_rebind(): reabre la sesion
 *      (threshold REBIND_AFTER_S)     Tailscale conservando claves, peers y
 *                                     VPN IP. Es el "reiniciar el servicio"
 *                                     pedido: sin reboot del ESP32.
 *                                     Se intenta UNA vez por episodio.
 *   3. reboot completo             -> esp_restart(): unico camino garantizado
 *      (threshold REBOOT_AFTER_S)     (stop+start del stack tenia un bug que
 *                                     dejaba la sesion en WIFI_WAIT).
 *
 * Metrica central: tiempo desde la ultima EVIDENCIA REAL de vida de la
 * tailnet, NO el state de microlink:
 *   - estado CONNECTED  -> milisegundos desde el ultimo byte RECIBIDO del
 *     control plane (microlink_get_ctrl_stale_ms). Un TCP half-open (p.ej.
 *     caida del enlace a las 4:38AM) deja el state en CONNECTED con los H2
 *     PING "enviandose" al buffer local: sin esta metrica el watchdog es
 *     ciego a la desconexion.
 *   - cualquier otro estado -> tiempo desde el ultimo CONNECTED observado.
 *
 * Solo actua cuando ya se observo ML_STATE_CONNECTED al menos una vez
 * (no dispara recovers espurios durante el bootstrap inicial).
 *
 * Rate-limit defensivo: si se dispara un reboot y por alguna razon no
 * se concreta, el watchdog espera al menos RECOVERY_COOLDOWN_MS antes
 * de intentar de nuevo.
 */

#define ML_WD_RECOVERY_COOLDOWN_MS  30000

typedef enum {
    ML_WD_RECOVERY_NONE = 0,
    ML_WD_RECOVERY_REBOOT,           /* esp_restart() llamado */
    ML_WD_RECOVERY_REBIND,           /* rebind suave de la sesion */
} microlink_wd_recovery_kind_t;

/* Snapshot atomico del estado del watchdog para /diag. */
typedef struct {
    bool enabled;
    bool ever_connected;               /* true si se vio CONNECTED >= 1 vez */
    uint32_t disconnect_ms;            /* ms sin evidencia real de vida de la
                                          tailnet (0 si sano) */
    uint32_t ctrl_rx_ms_ago;           /* ms desde el ultimo byte recibido del
                                          control plane (0 = nunca) */
    uint32_t total_recoveries;         /* total reboots disparados */
    uint32_t total_rebinds;            /* total rebinds suaves disparados */
    microlink_wd_recovery_kind_t last_recovery_kind;
    uint32_t last_recovery_ms_ago;     /* ms desde el ultimo reboot */
    uint32_t last_recovery_target_s;   /* umbral del ultimo reboot (segundos) */
    uint32_t last_rebind_ms_ago;       /* ms desde el ultimo rebind suave */
} microlink_wd_status_t;

/* Init: crea la task watchdog. Llamar una sola vez, despues de
 * microlink_wrapper_start() y telegram_init(). */
esp_err_t microlink_watchdog_init(void);

/* Snapshot atomico del estado. Seguro desde HTTP handler o serial task. */
void microlink_watchdog_get_status(microlink_wd_status_t *out);

/* Dispara un reboot manual. Usado por el comando serial "tailscale reset".
 * Notifica Telegram y luego llama esp_restart(). Bloqueante ~1-2s. */
esp_err_t microlink_watchdog_force_recover(void);

/* Serializa el status a string multi-linea (~320 bytes) para inclusion en
 * respuestas de Telegram o del /diag. out_size debe ser >= 360. */
void microlink_watchdog_format_status(char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
