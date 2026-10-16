#include "wake_logic.h"

#include <stdio.h>
#include <string.h>

void wake_logic_init(wake_logic_t *l, uint32_t now_ms, uint32_t window_ms) {
    if (l == NULL) return;
    memset(l, 0, sizeof(*l));
    l->window_ms = window_ms;
    (void)now_ms;
}

static void set_reason(wake_logic_t *l, const char *reason) {
    if (reason == NULL || *reason == 0) reason = "wake";
    strncpy(l->reason, reason, WAKE_LOGIC_REASON_LEN - 1);
    l->reason[WAKE_LOGIC_REASON_LEN - 1] = 0;
}

bool wake_logic_request(wake_logic_t *l, uint32_t now_ms, const char *reason) {
    if (l == NULL) return false;
    l->last_request_ms = now_ms;
    set_reason(l, reason);
    if (l->pending) {
        



        return false;
    }
    l->pending = true;
    l->armed_ms = now_ms;
    l->last_send_ms = 0;
    l->attempts = 0;
    return true;
}

bool wake_logic_due_send(const wake_logic_t *l, uint32_t now_ms, uint32_t retry_ms) {
    if (l == NULL || !l->pending) return false;
    if (l->attempts == 0) return true;
    return (uint32_t)(now_ms - l->last_send_ms) >= retry_ms;
}

void wake_logic_record_send(wake_logic_t *l, uint32_t now_ms) {
    if (l == NULL) return;
    l->attempts++;
    l->last_send_ms = now_ms;
}

void wake_logic_confirm(wake_logic_t *l, uint32_t now_ms) {
    if (l == NULL) return;
    l->pending = false;
    l->confirmed_ms = now_ms;
}

void wake_logic_fail(wake_logic_t *l, uint32_t now_ms) {
    if (l == NULL) return;
    l->pending = false;
    l->failed_ms = now_ms;
}

bool wake_logic_expired(const wake_logic_t *l, uint32_t now_ms) {
    if (l == NULL || !l->pending) return false;
    return (uint32_t)(now_ms - l->armed_ms) >= l->window_ms;
}

bool wake_logic_pending(const wake_logic_t *l) {
    return (l != NULL) && l->pending;
}

uint32_t wake_logic_elapsed_ms(const wake_logic_t *l, uint32_t now_ms) {
    if (l == NULL || !l->pending) return 0;
    return (uint32_t)(now_ms - l->armed_ms);
}

void wake_logic_format(const wake_logic_t *l, uint32_t now_ms,
                       char *out, size_t out_size) {
    if (out == NULL || out_size == 0) return;
    if (l == NULL) {
        snprintf(out, out_size, "wake: n/a");
        return;
    }
    if (l->pending) {
        snprintf(out, out_size,
                 "wake: PENDIENTE (%s) intentos=%u elapsed=%us",
                 l->reason[0] ? l->reason : "wake",
                 (unsigned)l->attempts,
                 (unsigned)((uint32_t)(now_ms - l->armed_ms) / 1000));
    } else if (l->confirmed_ms != 0 && l->confirmed_ms >= l->armed_ms) {
        snprintf(out, out_size, "wake: OK en %us tras %u intentos (%s)",
                 (unsigned)((l->confirmed_ms - l->armed_ms) / 1000),
                 (unsigned)l->attempts,
                 l->reason[0] ? l->reason : "wake");
    } else if (l->failed_ms != 0 && l->failed_ms >= l->armed_ms) {
        snprintf(out, out_size, "wake: FALLIDO tras %u intentos (%s)",
                 (unsigned)l->attempts,
                 l->reason[0] ? l->reason : "wake");
    } else {
        snprintf(out, out_size, "wake: inactivo");
    }
}
