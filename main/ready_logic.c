#include "ready_logic.h"

#include <stdio.h>
#include <string.h>

void ready_logic_init(ready_logic_t *l) {
    if (l == NULL) return;
    memset(l, 0, sizeof(*l));
    l->last_status = -1;
}

void ready_logic_arm(ready_logic_t *l, uint32_t now_ms, uint32_t hold_ms) {
    if (l == NULL) return;
    uint32_t until = now_ms + hold_ms;
    // Renovar la ventana nunca la acorta: un cliente que recarga cada 5 s mantiene
    // el sondeo rapido mientras siga esperando.
    if ((int32_t)(until - l->arm_until_ms) > 0) l->arm_until_ms = until;
}

bool ready_logic_armed(const ready_logic_t *l, uint32_t now_ms) {
    if (l == NULL) return false;
    return (int32_t)(now_ms - l->arm_until_ms) < 0;
}

bool ready_logic_due_probe(const ready_logic_t *l, uint32_t now_ms,
                           uint32_t fast_ms, uint32_t slow_ms) {
    if (l == NULL) return false;
    if (l->last_probe_ms == 0) return true;
    uint32_t gap = ready_logic_armed(l, now_ms) ? fast_ms : slow_ms;
    return (uint32_t)(now_ms - l->last_probe_ms) >= gap;
}

void ready_logic_record(ready_logic_t *l, uint32_t now_ms, bool ok,
                        int32_t status, uint32_t dur_ms) {
    if (l == NULL) return;
    l->probes++;
    l->last_probe_ms = now_ms;
    l->last_status = status;
    l->last_duration_ms = dur_ms;
    if (ok) {
        l->ok++;
        l->consec_fail = 0;
        l->last_ok_ms = now_ms;
        l->latched_ms = now_ms;
        l->latched = true;
    } else {
        l->err++;
        // Una sonda fallida NO apaga el latch, ni dos seguidas. Medido en campo el
        // 2026-10-15: con la maquina recien encendida y saturada por Docker, la
        // sonda al backend por la LAN da falsos negativos y el latch se caia y
        // voltaba a encenderse, costandonos viajes extra del cliente.
        //
        // Quien declara que server se fue es el poll de estado (state.c), que lo
        // verifica por otra via (:22 + /state) y hace que ready_probe.c invalide el
        // latch en <=2.5 s con el portal esperando. El TTL de 60 s queda de red de
        // seguridad para el caso zombie (poll dice ON pero nada responde).
        if (l->consec_fail < 0xFFFFFFFFUL) l->consec_fail++;
    }
}

bool ready_logic_is_ready(const ready_logic_t *l, uint32_t now_ms, uint32_t ttl_ms) {
    // server declaro rota su ruta publica: su palabra vale mas que nuestra sonda,
    // que ve el backend por la LAN y no puede saber si el tunel responde.
    if (l != NULL && l->public_broken) return false;
    if (l == NULL || !l->latched) return false;
    if (l->latched_ms == 0) return false;
    return (uint32_t)(now_ms - l->latched_ms) <= ttl_ms;
}

void ready_logic_set_public_broken(ready_logic_t *l, bool broken) {
    if (l == NULL) return;
    l->public_broken = broken;
    if (broken) l->latched = false;
}

void ready_logic_invalidate(ready_logic_t *l, uint32_t now_ms) {
    (void)now_ms;
    if (l == NULL) return;
    l->latched = false;
    l->latched_ms = 0;
}

void ready_logic_format(const ready_logic_t *l, uint32_t now_ms,
                        char *out, size_t out_size) {
    if (out == NULL || out_size == 0) return;
    if (l == NULL) {
        snprintf(out, out_size, "ready: n/a");
        return;
    }
    if (ready_logic_is_ready(l, now_ms, READY_LOGIC_TTL_MS)) {
        snprintf(out, out_size,
                 "ready: OK hace %us (sonda %ums HTTP %ld)",
                 (unsigned)((now_ms - l->latched_ms) / 1000),
                 (unsigned)l->last_duration_ms,
                 (long)l->last_status);
    } else if (ready_logic_armed(l, now_ms)) {
        snprintf(out, out_size, "ready: esperando (%u sondas, %u ok, %u err)",
                 (unsigned)l->probes, (unsigned)l->ok, (unsigned)l->err);
    } else {
        snprintf(out, out_size, "ready: inactivo (%u sondas, %u ok)",
                 (unsigned)l->probes, (unsigned)l->ok);
    }
}
