#include "wake.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "automation.h"
#include "cocopa01_client.h"
#include "state.h"
#include "telegram.h"
#include "wake_logic.h"
#include "wol.h"

static const char *TAG = "wake";

// Estado de la maquina (wake_logic.c, testeable en host).
static wake_logic_t s_wake;
// Tarea del supervisor; NULL hasta wake_init() o si el create falla.
static TaskHandle_t s_task = NULL;
// Ultimo aviso de fallo enviado por Telegram (para el rate limit).
static volatile uint32_t s_last_alert_ms = 0;
// Snapshot legible para /status y /diag (lo escribe la task, lo leen los
// handlers: datos Small, caben en un critical section).
static char s_status_text[96] = "wake: inactivo";

static uint32_t now_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void set_status_text(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_status_text, sizeof(s_status_text), fmt, ap);
    va_end(ap);
}

// true si cocopa01 responde de verdad ahora mismo (no es el cache del poll:
// aqui la decision de "ya desperto" tiene que ser en vivo, o el supervisor
// seguiria mandando magic packets 30 s despues de que el server arranco).
static bool cocopa01_alive_now(void) {
    return cocopa01_tcp_alive(WAKE_PROBE_MS);
}

static void alert_failure(uint32_t now, const wake_logic_t *l) {
    if (now - s_last_alert_ms < WAKE_ALERT_COOLDOWN_MS) return;
    s_last_alert_ms = now;

    char msg[320];
    snprintf(msg, sizeof(msg),
        "AVISO Auto-WOL: cocopa01 NO respondio tras %u rafagas en %u min "
        "(motivo: %s).\n"
        "El gateway sigue vivo y reintentara solo en el proximo acceso. "
        "Si el negocio sigue caido: revisar alimentacion/BIOS/ethernet de cocopa01.",
        (unsigned)l->attempts,
        (unsigned)(l->window_ms / 60000),
        l->reason[0] ? l->reason : "wake");
    ESP_LOGE(TAG, "%s", msg);
    telegram_broadcast(msg);
}

static void wake_task(void *arg) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WAKE_TICK_MS));

        uint32_t now = now_ms();
        if (!wake_logic_pending(&s_wake)) continue;

        // Mientras haya un intento abierto mantenemos el sondeo rapido de
        // state_poll_task (2.5 s) para que /diag refleje el estado real del
        // arranque sin esperar los 30 s del ciclo normal.
        state_trigger_fast_poll();

        // 1) Ya respondio? -> fin del intento en exito.
        if (cocopa01_alive_now()) {
            uint32_t attempts = s_wake.attempts;
            uint32_t elapsed = (uint32_t)(now - s_wake.armed_ms);
            wake_logic_confirm(&s_wake, now);
            ESP_LOGI(TAG, "wake OK: cocopa01 respondio en %u s tras %u rafaga(s) (%s)",
                     (unsigned)(elapsed / 1000), (unsigned)attempts,
                     s_wake.reason[0] ? s_wake.reason : "wake");
            set_status_text("wake: OK en %us tras %u intentos (%s)",
                            (unsigned)(elapsed / 1000), (unsigned)attempts,
                            s_wake.reason[0] ? s_wake.reason : "wake");
            // Alguien esta esperando: el reloj de inactividad vuelve a cero.
            automation_kick();
            continue;
        }

        // 2) Ventana agotada -> fin del intento como fallido + aviso.
        if (wake_logic_expired(&s_wake, now)) {
            uint32_t attempts = s_wake.attempts;
            uint32_t elapsed = (uint32_t)(now - s_wake.armed_ms);
            wake_logic_fail(&s_wake, now);
            ESP_LOGE(TAG, "wake FALLIDO: %u rafagas en %u s sin respuesta (%s)",
                     (unsigned)attempts, (unsigned)(elapsed / 1000),
                     s_wake.reason[0] ? s_wake.reason : "wake");
            set_status_text("wake: FALLIDO tras %u intentos en %us (%s)",
                            (unsigned)attempts, (unsigned)(elapsed / 1000),
                            s_wake.reason[0] ? s_wake.reason : "wake");
            alert_failure(now, &s_wake);
            continue;
        }

        // 3) Toca reintentar.
        if (wake_logic_due_send(&s_wake, now, WAKE_RETRY_MS)) {
            esp_err_t err = wol_send_default();
            wake_logic_record_send(&s_wake, now);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "wake: rafaga %u no pudo enviar (%s); se reintenta",
                         (unsigned)s_wake.attempts, esp_err_to_name(err));
                set_status_text("wake: PENDIENTE (%s) intentos=%u sin red",
                                s_wake.reason[0] ? s_wake.reason : "wake",
                                (unsigned)s_wake.attempts);
            } else {
                ESP_LOGI(TAG, "wake: rafaga %u enviada (%s)",
                         (unsigned)s_wake.attempts,
                         s_wake.reason[0] ? s_wake.reason : "wake");
                set_status_text("wake: PENDIENTE (%s) intentos=%u elapsed=%us",
                                s_wake.reason[0] ? s_wake.reason : "wake",
                                (unsigned)s_wake.attempts,
                                (unsigned)((now - s_wake.armed_ms) / 1000));
            }
        }
    }
}

esp_err_t wake_init(void) {
    wake_logic_init(&s_wake, now_ms(), WAKE_WINDOW_MS);
    // El ARP estatico del servidor queda asegurado desde el boot: el unicast
    // del WOL y las sondas TCP salen con la MAC correcta aunque lwIP todavia
    // no tenga entrada para 192.168.1.100.
    esp_err_t arp = wol_prime_arp();
    if (arp != ESP_OK) {
        ESP_LOGW(TAG, "wol_prime_arp: %s (se reintentara en cada WOL)",
                 esp_err_to_name(arp));
    }

    BaseType_t ok = xTaskCreate(wake_task, "wake", 4096, NULL, 5, &s_task);
    if (ok != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "wake task create failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "wake supervisor listo (reintento cada %us, ventana %us)",
             (unsigned)(WAKE_RETRY_MS / 1000), (unsigned)(WAKE_WINDOW_MS / 1000));
    return ESP_OK;
}

esp_err_t wake_request(const char *reason) {
    uint32_t now = now_ms();
    bool fresh = wake_logic_request(&s_wake, now, reason);

    if (fresh) {
        ESP_LOGI(TAG, "wake solicitado (%s): rafaga inmediata + reintentos cada %us",
                 s_wake.reason[0] ? s_wake.reason : "wake",
                 (unsigned)(WAKE_RETRY_MS / 1000));
    } else {
        ESP_LOGD(TAG, "wake ya en curso (%s, %u rafagas)",
                 s_wake.reason[0] ? s_wake.reason : "wake",
                 (unsigned)s_wake.attempts);
    }

    if (s_task == NULL) {
        // Degradacion: si la task no existe (fallo de xTaskCreate) al menos
        // mandamos la rafaga puntual, como hacia el firmware <= 1.8.2.
        esp_err_t err = wol_send_default();
        wake_logic_record_send(&s_wake, now);
        return err;
    }

    // Fast poll para que /diag refleje el arranque en <=2.5 s.
    state_trigger_fast_poll();
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

void wake_cancel(void) {
    if (!wake_logic_pending(&s_wake)) return;
    wake_logic_fail(&s_wake, now_ms());
    ESP_LOGI(TAG, "wake cancelado por el sistema (%s)",
             s_wake.reason[0] ? s_wake.reason : "wake");
    set_status_text("wake: cancelado (%s)", s_wake.reason[0] ? s_wake.reason : "wake");
}

void wake_get_status(wake_status_t *out) {
    if (out == NULL) return;
    uint32_t now = now_ms();
    memset(out, 0, sizeof(*out));
    out->pending = wake_logic_pending(&s_wake);
    out->attempts = s_wake.attempts;
    out->elapsed_ms = wake_logic_elapsed_ms(&s_wake, now);
    out->window_ms = WAKE_WINDOW_MS;
    out->retry_ms = WAKE_RETRY_MS;
    snprintf(out->text, sizeof(out->text), "%s", s_status_text);
}

const char *wake_status_text(void) {
    return s_status_text;
}
