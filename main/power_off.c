#include "power_off.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdlib.h>
#include <string.h>

#include "server_client.h"
#include "power_event.h"
#include "power_off_logic.h"
#include "state.h"
#include "system_health.h"
#include "telegram.h"
#include "wol.h"

static const char *TAG = "power_off";

// Espera maxima a que server responda tras el WOL. Un arranque en frio
// desde S5 tarda ~26-30 s (medido) y un resume desde S3 ~5-10 s; 90 s deja
// margen sin alargar la espera del usuario mas de lo necesario.
#define POWER_OFF_WAIT_MAX_MS   90000UL
#define POWER_OFF_POLL_MS        2000UL
// Ventana para confirmar por sonda un apagado/suspension cuya respuesta HTTP
// no llego (el daemon apaga la maquina antes de responder).
#define POWER_OFF_VERIFY_MS      6000UL
// Timeout de la sonda TCP a :22.
#define POWER_OFF_PROBE_MS       1200

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile power_off_result_t s_last_result = POWER_OFF_RESULT_ALREADY_OFF;
static volatile uint32_t s_last_elapsed_ms = 0;
static volatile bool     s_last_woke_up = false;
static char              s_last_detail[112] = "sin ejecuciones";
static volatile bool     s_async_running = false;

static uint32_t now_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void save_report(const power_off_report_t *rep) {
    portENTER_CRITICAL(&s_mux);
    s_last_result    = rep->result;
    s_last_elapsed_ms = rep->elapsed_ms;
    s_last_woke_up   = rep->woke_up;
    memcpy(s_last_detail, rep->detail, sizeof(s_last_detail));
    s_last_detail[sizeof(s_last_detail) - 1] = 0;
    portEXIT_CRITICAL(&s_mux);
}

void power_off_status_str(char *out, size_t out_size) {
    if (out == NULL || out_size == 0) return;
    power_off_result_t result;
    uint32_t elapsed;
    bool woke;
    char detail[112];
    portENTER_CRITICAL(&s_mux);
    result   = s_last_result;
    elapsed  = s_last_elapsed_ms;
    woke     = s_last_woke_up;
    memcpy(detail, s_last_detail, sizeof(detail));
    detail[sizeof(detail) - 1] = 0;
    portEXIT_CRITICAL(&s_mux);

    const char *label = "fallo";
    switch (result) {
        case POWER_OFF_RESULT_OK:          label = "OK"; break;
        case POWER_OFF_RESULT_ALREADY_OFF: label = "ya apagado"; break;
        default: break;
    }
    snprintf(out, out_size,
             "power-off: %s en %u s%s\n  %s",
             label, (unsigned)(elapsed / 1000),
             woke ? " (despertado con WOL)" : "",
             detail);
}

// Espera a que server vuelva a responder en :22. Emite latidos para que
// el supervisor de salud no reinicie el ESP32 durante la espera.
static bool wait_for_alive(uint32_t max_ms, health_channel_t hb) {
    uint32_t start = now_ms();
    while ((uint32_t)(now_ms() - start) < max_ms) {
        vTaskDelay(pdMS_TO_TICKS(POWER_OFF_POLL_MS));
        system_health_heartbeat(hb);
        if (server_tcp_alive(POWER_OFF_PROBE_MS)) return true;
    }
    return false;
}

bool power_off_wait_until_down(uint32_t timeout_ms, health_channel_t hb) {
    uint32_t start = now_ms();
    while ((uint32_t)(now_ms() - start) < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(500));
        system_health_heartbeat(hb);
        if (!server_tcp_alive(700)) return true;
    }
    return false;
}

// Confirma por sonda que server dejo de responder. Se usa cuando el POST
// /off (o /suspend) no devolvio respuesta: el daemon ejecuta la accion
// ANTES de responder y la maquina se apaga/suspende en el camino, asi que la
// respuesta HTTP se pierde con mucha frecuencia.
static bool confirm_shutdown(const char *reason, health_channel_t hb) {
    if (!power_off_wait_until_down(POWER_OFF_VERIFY_MS, hb)) return false;
    ESP_LOGI(TAG, "%s: apagado confirmado por sonda (la respuesta HTTP se perdio)",
             reason);
    return true;
}

esp_err_t power_off_run(const char *reason, health_channel_t hb,
                        power_off_report_t *rep) {
    power_off_report_t local;
    memset(&local, 0, sizeof(local));
    local.result = POWER_OFF_RESULT_FAIL;

    uint32_t t0 = now_ms();
    bool alive = server_tcp_alive(POWER_OFF_PROBE_MS);
    bool suspect = state_suspect_suspended();

    ESP_LOGI(TAG, "%s: secuencia de apagado (tcp_alive=%d, rastro_suspension=%d)",
             reason, (int)alive, (int)suspect);

    if (!alive) {
        // El daemon puede seguir vivo aunque sshd no responda: el POST se
        // intenta igual (barato, sin efectos secundarios).
        if (server_send_off() == ESP_OK) {
            local.result = POWER_OFF_RESULT_OK;
            snprintf(local.detail, sizeof(local.detail),
                     "apagado confirmado por el daemon (:9999)");
            goto done;
        }
        power_off_decision_t dec = power_off_decide(false, suspect);
        if (dec == POWER_OFF_DECISION_ALREADY_OFF) {
            // No responde y no hay ningun rastro de que estuviera suspendido:
            // esta apagado de verdad. NO lo despertamos: un magic packet a un
            // equipo apagado lo enciende (y con el, todo el stack de Docker).
            local.result = POWER_OFF_RESULT_ALREADY_OFF;
            snprintf(local.detail, sizeof(local.detail),
                     "no responde y no hay rastro de suspension: se asume apagado");
            goto done;
        }
        // Hay rastro de suspension: esta en S3 (la NIC caida, por eso no
        // responde). ACPI no permite S3 -> S5 directo: hay que despertar a
        // S0 con un magic packet y ahi si mandar /off.
        ESP_LOGI(TAG, "%s: server no responde pero hay rastro de S3: WOL + /off",
                 reason);
        if (wol_send_default() != ESP_OK) {
            snprintf(local.detail, sizeof(local.detail),
                     "no se pudo enviar el magic packet para despertarlo");
            goto done;
        }
        local.woke_up = true;
        if (!wait_for_alive(POWER_OFF_WAIT_MAX_MS, hb)) {
            local.result = POWER_OFF_RESULT_ALREADY_OFF;
            snprintf(local.detail, sizeof(local.detail),
                     "no respondio tras el WOL (%u s): se asume apagado",
                     (unsigned)(POWER_OFF_WAIT_MAX_MS / 1000));
            goto done;
        }
        ESP_LOGI(TAG, "%s: server despierto, mando /off", reason);
    }

    // El servidor responde: mandamos el POST /off.
    if (server_send_off() == ESP_OK) {
        local.result = POWER_OFF_RESULT_OK;
        snprintf(local.detail, sizeof(local.detail),
                 "apagado confirmado por el daemon (:9999)");
        goto done;
    }
    // El POST no volvio. Puede ser que el equipo se haya apagado y la
    // respuesta se perdiera en el camino: lo confirmamos por sonda.
    if (confirm_shutdown(reason, hb)) {
        local.result = POWER_OFF_RESULT_OK;
        snprintf(local.detail, sizeof(local.detail),
                 "apagado verificado por sonda (la respuesta HTTP se perdio)");
        goto done;
    }
    local.result = POWER_OFF_RESULT_FAIL;
    snprintf(local.detail, sizeof(local.detail),
             "el daemon no confirmo el apagado y el servidor sigue respondiendo");

done:
    local.elapsed_ms = (uint32_t)(now_ms() - t0);
    if (local.result == POWER_OFF_RESULT_OK || local.result == POWER_OFF_RESULT_ALREADY_OFF) {
        state_set(SERVER_STATE_OFF);
    }
    if (local.result == POWER_OFF_RESULT_OK) {
        // El servidor quedo apagado de verdad (S5): el rastro de suspension
        // se limpia. Si se dejara, un /off posterior creeria que sigue en S3
        // y lo despertaria (arrancando todo el stack) para volver a apagarlo.
        state_clear_suspect_suspended();
        // Apagado DELIBERADO: el vigilante de cortes no debe intentar
        // devolverlo a la vida si despues hay un corte de luz.
        power_event_note_intentional_off();
    }
    if (rep != NULL) *rep = local;
    save_report(&local);
    ESP_LOGI(TAG, "%s: resultado=%d en %u s (%s)",
             reason, (int)local.result, (unsigned)(local.elapsed_ms / 1000),
             local.detail);
    return (local.result == POWER_OFF_RESULT_FAIL) ? ESP_FAIL : ESP_OK;
}

// ─── Variante asincrona (Telegram) ─────────────────────────────

typedef struct {
    char    reason[32];
    int64_t chat_id;
} async_ctx_t;

static void async_task(void *arg) {
    async_ctx_t *ctx = (async_ctx_t *)arg;
    power_off_report_t rep;
    power_off_run(ctx->reason, HEALTH_CHANNEL_POWER_OFF, &rep);

    char msg[320];
    switch (rep.result) {
        case POWER_OFF_RESULT_OK:
            snprintf(msg, sizeof(msg),
                     "%s: OK - server apagado%s.",
                     ctx->reason,
                     rep.woke_up ? " (se despertó de S3 y se apagó)" : "");
            break;
        case POWER_OFF_RESULT_ALREADY_OFF:
            snprintf(msg, sizeof(msg),
                     "%s: server no respondio y no hay rastro de suspension: "
                     "se asume que ya estaba apagado.", ctx->reason);
            break;
        default:
            snprintf(msg, sizeof(msg),
                     "%s: NO se pudo confirmar el apagado (%s). "
                     "Revisar el daemon en server.", ctx->reason, rep.detail);
            break;
    }
    telegram_send_text(ctx->chat_id, msg);
    free(ctx);
    s_async_running = false;
    // El canal solo debe estar activo mientras la task existe: si queda
    // habilitado, el supervisor veria "sin latido" al terminar y reiniciaria
    // el ESP32 sin motivo.
    system_health_enable_channel(HEALTH_CHANNEL_POWER_OFF, false);
    vTaskDelete(NULL);
}

esp_err_t power_off_start_async(const char *reason, int64_t chat_id) {
    if (s_async_running) return ESP_ERR_INVALID_STATE;
    async_ctx_t *ctx = (async_ctx_t *)calloc(1, sizeof(async_ctx_t));
    if (ctx == NULL) return ESP_ERR_NO_MEM;
    snprintf(ctx->reason, sizeof(ctx->reason), "%s", reason ? reason : "off");
    ctx->chat_id = chat_id;
    s_async_running = true;
    BaseType_t ok = xTaskCreate(async_task, "power_off", 4096, ctx, 5, NULL);
    if (ok != pdPASS) {
        s_async_running = false;
        free(ctx);
        return ESP_FAIL;
    }
    return ESP_OK;
}
