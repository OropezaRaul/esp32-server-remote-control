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
// Ventana para confirmar por sonda un apagado/suspension (tiempo prudente para
// que el sistema operativo complete el shutdown y corte la red).
//
// Medido en server (2026-10-30): el daemon acepta el POST /off en 3 ms y
// systemd para todos los servicios en ~2 s, pero la maquina tarda ~30 s mas
// en cortar la corriente (systemd-shutdown espera hasta su --timeout y luego
// desmonta y sincroniza). Con la ventana de 20 s el ESP32 reportaba "no se
// pudo confirmar" en apagados que si funcionaban, y el usuario se quedaba
// sin saber si habia que mandarlo otra vez. 60 s cubre el peor caso medido.
#define POWER_OFF_VERIFY_MS     60000UL
// Margen de cortesia cuando el servidor NO responde pero lo vimos ON hace
// poco: puede estar arrancando (tras un /wake, sshd tarda 20-30 s en
// levantar) o apagandose. Antes el ESP32 contestaba "ya esta apagado" sin
// hacer NADA en cuanto la sonda a :22 fallaba, y el /off del usuario se
// perdia en silencio.
#define POWER_OFF_GRACE_MS      45000UL
// Timeout de la sonda TCP a :22.
#define POWER_OFF_PROBE_MS       1200

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile power_off_result_t s_last_result = POWER_OFF_RESULT_ALREADY_OFF;
static volatile uint32_t s_last_elapsed_ms = 0;
static volatile bool     s_last_woke_up = false;
static char              s_last_detail[112] = "sin ejecuciones";
static volatile bool     s_async_running = false;
static volatile bool     s_cancel_requested = false;

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

// ─── Sondeos de vida ─────────────────────────────────────────────
//
// server tiene DOS canales de vida y hay que mirar los DOS:
//   :9999 -> el daemon nativo, el UNICO que puede ejecutar el apagado.
//   :22   -> sshd, solo indicador de vida (y tarda 20-30 s en un arranque en
//            frio, asi que es el que miente justo cuando hay que actuar).
//
// Decidir solo con :22 fue el bug que reporto el usuario: durante un
// arranque (o con sshd lento) el /off de Telegram contestaba "ya esta
// apagado" sin enviar nada, y el servidor se quedaba encendido.
static bool server_reachable(void) {
    if (server_daemon_alive(POWER_OFF_PROBE_MS)) return true;
    return server_tcp_alive(POWER_OFF_PROBE_MS);
}

// true cuando server no responde por NINGUN canal: dejo de servir.
static bool server_gone(void) {
    if (server_daemon_alive(POWER_OFF_PROBE_MS)) return false;
    if (server_tcp_alive(POWER_OFF_PROBE_MS)) return false;
    return true;
}

// Espera a que server vuelva a responder por algun canal. Emite latidos
// para que el supervisor de salud no reinicie el ESP32 durante la espera.
static bool wait_for_alive(uint32_t max_ms, health_channel_t hb) {
    uint32_t start = now_ms();
    uint32_t last_wol = start;
    while ((uint32_t)(now_ms() - start) < max_ms) {
        if (s_cancel_requested) return false;
        vTaskDelay(pdMS_TO_TICKS(POWER_OFF_POLL_MS));
        system_health_heartbeat(hb);
        if (s_cancel_requested) return false;
        if (server_reachable()) return true;
        // Reintentar WOL cada 10 s mientras esperamos el despertar de S3
        if ((uint32_t)(now_ms() - last_wol) >= 10000UL) {
            wol_send_default();
            last_wol = now_ms();
        }
    }
    return false;
}

// Margen de cortesia: el servidor no responde, pero lo vimos ON hace poco
// (puede estar arrancando o apagandose). Esperamos a que conteste antes de
// decir "ya esta apagado".
static bool wait_until_reachable(uint32_t max_ms, health_channel_t hb) {
    uint32_t start = now_ms();
    while ((uint32_t)(now_ms() - start) < max_ms) {
        if (s_cancel_requested) return false;
        vTaskDelay(pdMS_TO_TICKS(POWER_OFF_POLL_MS));
        system_health_heartbeat(hb);
        if (s_cancel_requested) return false;
        if (server_reachable()) return true;
    }
    return false;
}

bool power_off_wait_until_down(uint32_t timeout_ms, health_channel_t hb) {
    uint32_t start = now_ms();
    int consecutive_down = 0;
    while ((uint32_t)(now_ms() - start) < timeout_ms) {
        if (s_cancel_requested) return false;
        vTaskDelay(pdMS_TO_TICKS(800));
        system_health_heartbeat(hb);
        if (server_gone()) {
            consecutive_down++;
            if (consecutive_down >= 2) return true;
        } else {
            consecutive_down = 0;
        }
    }
    return false;
}

// Confirma por sonda que server dejo de responder. Se usa para verificar
// el apagado real tanto si el POST /off devolvio 200 como si la respuesta
// HTTP se perdio porque la maquina se apago en el camino.
static bool confirm_shutdown(const char *reason, health_channel_t hb) {
    if (!power_off_wait_until_down(POWER_OFF_VERIFY_MS, hb)) return false;
    ESP_LOGI(TAG, "%s: apagado confirmado por sonda (dejo de responder)",
             reason);
    return true;
}

esp_err_t power_off_run(const char *reason, health_channel_t hb,
                        power_off_report_t *rep) {
    power_off_report_t local;
    memset(&local, 0, sizeof(local));
    local.result = POWER_OFF_RESULT_FAIL;

    uint32_t t0 = now_ms();
    bool alive = server_reachable();
    bool suspect = state_suspect_suspended();

    ESP_LOGI(TAG, "%s: secuencia de apagado (vive=%d, rastro_suspension=%d, estado=%s)",
             reason, (int)alive, (int)suspect, server_state_str(state_get()));

    if (!alive) {
        // Margen de cortesia: si lo vimos ON hace nada, el equipo puede estar
        // arrancando (el usuario acaba de pedir /wake) o apagandose. Preguntar
        // solo por :22 durante un arranque en frio es perder la carrera
        // (sshd tarda 20-30 s): ahi el ESP32 creia que estaba apagado y el
        // /off del usuario no hacia NADA.
        bool recently_on = (state_get() == SERVER_STATE_ON) ||
                           state_last_poll_tcp_alive();
        if (recently_on && !suspect) {
            ESP_LOGI(TAG, "%s: no responde pero lo vimos ON hace poco: espero hasta %u s",
                     reason, (unsigned)(POWER_OFF_GRACE_MS / 1000));
            if (wait_until_reachable(POWER_OFF_GRACE_MS, hb)) {
                alive = true;
                ESP_LOGI(TAG, "%s: contesto durante el margen de cortesia", reason);
            }
        }
    }

    if (!alive) {
        // El daemon puede seguir vivo aunque las sondas fallen (o al revés):
        // el POST se intenta igual (barato, sin efectos secundarios si no hay
        // nadie escuchando) y de paso ES la prueba definitiva.
        if (server_send_off() == ESP_OK) {
            state_note_shutdown_sent();
            if (confirm_shutdown(reason, hb)) {
                local.result = POWER_OFF_RESULT_OK;
                snprintf(local.detail, sizeof(local.detail),
                         "apagado confirmado por sonda (:9999)");
                goto done;
            }
            local.result = POWER_OFF_RESULT_FAIL;
            snprintf(local.detail, sizeof(local.detail),
                     "se envió /off pero el servidor sigue respondiendo");
            goto done;
        }
        power_off_decision_t dec = power_off_decide(false, suspect);
        if (dec == POWER_OFF_DECISION_ALREADY_OFF) {
            // No responde por ningun canal y no hay ningun rastro de que
            // estuviera suspendido: esta apagado de verdad. NO lo despertamos:
            // un magic packet a un equipo apagado lo enciende (y con el, todo
            // el stack de Docker).
            local.result = POWER_OFF_RESULT_ALREADY_OFF;
            snprintf(local.detail, sizeof(local.detail),
                     "no responde por ningún canal y no hay rastro de suspensión: se asume apagado");
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
            if (s_cancel_requested) {
                snprintf(local.detail, sizeof(local.detail), "secuencia cancelada por usuario");
                goto done;
            }
            local.result = POWER_OFF_RESULT_ALREADY_OFF;
            snprintf(local.detail, sizeof(local.detail),
                     "no respondio tras el WOL (%u s): se asume apagado",
                     (unsigned)(POWER_OFF_WAIT_MAX_MS / 1000));
            goto done;
        }
        if (s_cancel_requested) {
            snprintf(local.detail, sizeof(local.detail), "secuencia cancelada por usuario");
            goto done;
        }
        ESP_LOGI(TAG, "%s: server despierto, mando /off", reason);
    }

    // El servidor responde: mandamos el POST /off.
    if (server_send_off() == ESP_OK) {
        state_note_shutdown_sent();
        // El daemon acepto el comando de apagado. Confirmamos por sonda que la
        // maquina efectivamente se apague (deje de responder TCP).
        if (confirm_shutdown(reason, hb)) {
            local.result = POWER_OFF_RESULT_OK;
            snprintf(local.detail, sizeof(local.detail),
                     "apagado confirmado por sonda (:9999)");
            goto done;
        }
        local.result = POWER_OFF_RESULT_FAIL;
        snprintf(local.detail, sizeof(local.detail),
                 "se envió /off pero el servidor sigue respondiendo (no se apagó)");
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
        state_trigger_fast_poll();
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

    if (s_cancel_requested) {
        ESP_LOGI(TAG, "async power_off cancelado por /wake u otra orden");
        free(ctx);
        s_async_running = false;
        s_cancel_requested = false;
        system_health_enable_channel(HEALTH_CHANNEL_POWER_OFF, false);
        vTaskDelete(NULL);
        return;
    }

    char msg[320];
    unsigned secs = (unsigned)(rep.elapsed_ms / 1000);
    switch (rep.result) {
        case POWER_OFF_RESULT_OK:
            snprintf(msg, sizeof(msg),
                     "%s: OK - server apagado y verificado en %u s%s.",
                     ctx->reason, secs,
                     rep.woke_up ? " (se despertó de S3 y se apagó)" : "");
            break;
        case POWER_OFF_RESULT_ALREADY_OFF:
            snprintf(msg, sizeof(msg),
                     "%s: server no responde por ningún canal y no hay rastro "
                     "de suspensión: se asume que ya estaba apagado (%u s).",
                     ctx->reason, secs);
            break;
        default:
            snprintf(msg, sizeof(msg),
                     "%s: NO se pudo confirmar el apagado tras %u s (%s). "
                     "El servidor sigue respondiendo.", ctx->reason, secs, rep.detail);
            break;
    }
    // El resultado se avisa por Telegram cuando hay chat_id ( Telegram /
    // automation / scheduler ). El comando de serie usa la misma secuencia con
    // chat_id = 0: entonces solo queda en el log del puerto serie.
    if (ctx->chat_id > 0) {
        telegram_send_text(ctx->chat_id, msg);
    } else {
        ESP_LOGI(TAG, "%s", msg);
    }
    // Pila libre que queda tras el envio (telegram_send_text hace un handshake
    // TLS: necesita varios KB de pila). Con la pila de 4096 bytes que tenia
    // esta task, ese envio desbordaba la pila -> panic -> el ESP32 se
    // reiniciaba JUSTO en el /off (y el usuario nunca recibia el mensaje
    // final). El high-water mark deja el margen real a la vista.
    ESP_LOGI(TAG, "pila libre tras el envio: %u bytes",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    free(ctx);
    s_async_running = false;
    s_cancel_requested = false;
    // El canal solo debe estar activo mientras la task existe: si queda
    // habilitado, el supervisor veria "sin latido" al terminar y reiniciaria
    // el ESP32 sin motivo.
    system_health_enable_channel(HEALTH_CHANNEL_POWER_OFF, false);
    vTaskDelete(NULL);
}

esp_err_t power_off_start_async(const char *reason, int64_t chat_id) {
    if (s_async_running) return ESP_ERR_INVALID_STATE;
    s_cancel_requested = false;
    async_ctx_t *ctx = (async_ctx_t *)calloc(1, sizeof(async_ctx_t));
    if (ctx == NULL) return ESP_ERR_NO_MEM;
    snprintf(ctx->reason, sizeof(ctx->reason), "%s", reason ? reason : "off");
    ctx->chat_id = chat_id;
    s_async_running = true;
    // Pila: telegram_send_text() hace un handshake TLS (mbedtls) que necesita
    // varios KB. Con 4096 bytes esta task desbordaba la pila al enviar el
    // mensaje final -> panic -> el ESP32 se reiniciaba en pleno /off. El
    // task "telegram" usa 16384; aqui 8192 basta con holgura (queda el
    // high-water mark en el log para verificarlo).
    BaseType_t ok = xTaskCreate(async_task, "power_off", 8192, ctx, 5, NULL);
    if (ok != pdPASS) {
        s_async_running = false;
        free(ctx);
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool power_off_is_running(void) {
    return s_async_running;
}

void power_off_cancel(void) {
    if (s_async_running) {
        ESP_LOGI(TAG, "power_off_cancel: solicitando cancelacion de tarea asincrona");
        s_cancel_requested = true;
    }
}
