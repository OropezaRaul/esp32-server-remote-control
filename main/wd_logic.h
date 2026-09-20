#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * wd_logic — Decision core del watchdog de Tailscale (sin dependencias).
 *
 * Modulo PURO (sin FreeRTOS, sin ESP-IDF, sin microlink): recibe un snapshot
 * de hechos y devuelve la siguiente accion de la escalera de recuperacion:
 *
 *   saludable  -> WD_ACT_NONE
 *   ~50% umbral-> WD_ACT_PRE_WARN (solo log)
 *   >=rebind   -> WD_ACT_REBIND  (reintentar sesion sin reboot; 1x/episodio)
 *   >=reboot   -> WD_ACT_REBOOT  (esp_restart, accion garantizada)
 *
 * La diferencia clave contra la version anterior: la metrica central ya no
 * es "el state de microlink dice CONNECTED" (que puede quedarse congelado
 * en CONNECTED durante una sesion muerta por TCP half-open), sino el tiempo
 * desde la ULTIMA EVIDENCIA REAL de vida (bytes recibidos del control plane
 * cuando state==CONNECTED; ultimo CONNECTED cuando no).
 *
 * Compila tanto en el firmware como en host (gcc) para testear el algoritmo.
 * ========================================================================== */

typedef enum {
    WD_ACT_NONE = 0,
    WD_ACT_PRE_WARN,   /* cruzo la mitad del umbral de reboot: log */

    /* Escalera de recuperacion (de menor a mayor impacto): */
    WD_ACT_REBIND,     /* reiniciar la sesion Tailscale (reabrir sockets con
                          claves/peers/IP intactos) — equivalencia a
                          "reiniciar el servicio" sin reiniciar el ESP32 */
    WD_ACT_REBOOT,     /* esp_restart(): unico camino garantizado */
} wd_action_t;

typedef struct {
    /* --- Entradas (snapshot tomado por el caller) --- */
    bool ever_connected;        /* el watchdog vio CONNECTED >= 1 vez (armed) */
    bool state_connected;       /* microlink_wrapper_get_state() == CONNECTED */
    uint32_t last_connected_ms; /* tick del ultimo CONNECTED (0 = nunca) */
    uint32_t ctrl_stale_ms;     /* ms desde el ultimo byte recibido del
                                   control plane (0 = nunca recibido) */
    uint32_t tick;              /* ahora, en la base de tiempo del watchdog */
    uint32_t last_reboot_ms;    /* tick del ultimo REBOOT disparado (0=ninguno) */
    uint32_t last_rebind_ms;    /* tick del ultimo REBIND disparado (0=ninguno) */
    bool rebind_done;           /* rebind ya intentado en este episodio */
    uint32_t reboot_after_ms;   /* umbral de reboot completo */
    uint32_t rebind_after_ms;   /* umbral de rebind suave */
    uint32_t cooldown_ms;       /* antirebote de REBOOT (si esp_restart falla) */

    /* --- Salidas (rellenadas por wd_evaluate) --- */
    uint32_t disconnect_ms;     /* ms sin evidencia de vida de la tailnet */
    bool zombie;                /* state==CONNECTED pero control plane en
                                   silencio (sesion muerta no detectada) */
} wd_input_t;

/* Evalua el snapshot y devuelve la accion. Modifica solo los campos de
 * salida de `in`. Nunca bloquea. */
wd_action_t wd_evaluate(wd_input_t *in);

#ifdef __cplusplus
}
#endif
