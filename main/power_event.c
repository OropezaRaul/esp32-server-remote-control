#include "power_event.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <string.h>
#include <time.h>

#include "auto_reboot.h"
#include "server_client.h"
#include "power_event_logic.h"
#include "state.h"
#include "system_health.h"
#include "telegram.h"
#include "wake.h"

static const char *TAG = "power_event";

#define NVS_NAMESPACE        "power_event"
#define NVS_KEY_PHASE        "phase"
#define NVS_KEY_WAS_ON       "was_on"
#define NVS_KEY_NOTIFIED     "notified"
#define NVS_KEY_GAVE_UP      "gave_up"
#define NVS_KEY_ATTEMPTS     "attempts"
#define NVS_KEY_LAST_SEEN     "last_seen"

// Pila de la task: 8 KB. Con 4 KB el ESP32 la mataba con
// "A stack overflow in task power_event has been detected" y entraba en un
// bucle de reinicio (el firmware no llegaba ni a levantar el HTTP). En esta task
// se escribe en NVS (ruta de flash) y se manda Telegram (TLS), que es
// exactamente lo que hace la task de automation con sus 8 KB sin problema.
#define TASK_STACK 8192
#define TASK_PRIO  4
#define POLL_MS    5000

static power_event_t s_ev;
static bool s_initialized = false;
// Ultimo instante (reloj de pared) en que el ESP32 estuvo vivo. Al arrancar,
// la diferencia con la hora actual dice cuanto tiempo estuvo apagado: si fue
// mucho mas que su propio intervalo de auto-reboot, se apago por un corte.
static uint32_t s_last_seen_unix = 0;
static bool s_gap_checked = false;

static uint32_t now_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

// Persiste lo que no puede perderse: si el ESP32 se reinicia a mitad de un
// corte (el auto-reboot es horario), tiene que seguir esperando al servidor.
static void persist(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_PHASE, (uint8_t)s_ev.phase);
    nvs_set_u8(h, NVS_KEY_WAS_ON, s_ev.server_was_on ? 1 : 0);
    nvs_set_u8(h, NVS_KEY_NOTIFIED, s_ev.notified ? 1 : 0);
    nvs_set_u8(h, NVS_KEY_GAVE_UP, s_ev.gave_up ? 1 : 0);
    nvs_set_u32(h, NVS_KEY_ATTEMPTS, s_ev.attempts);
    nvs_commit(h);
    nvs_close(h);
}

static void load(void) {
    power_event_logic_init(&s_ev, now_ms());
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t phase = 0, was_on = 0, notified = 0, gave_up = 0;
    uint32_t attempts = 0;
    nvs_get_u8(h, NVS_KEY_PHASE, &phase);
    nvs_get_u8(h, NVS_KEY_WAS_ON, &was_on);
    nvs_get_u8(h, NVS_KEY_NOTIFIED, &notified);
    nvs_get_u8(h, NVS_KEY_GAVE_UP, &gave_up);
    nvs_get_u32(h, NVS_KEY_ATTEMPTS, &attempts);
    nvs_get_u32(h, NVS_KEY_LAST_SEEN, &s_last_seen_unix);
    nvs_close(h);
    s_ev.phase = (power_event_phase_t)phase;
    s_ev.server_was_on = (was_on != 0);
    s_ev.notified = (notified != 0);
    s_ev.gave_up = (gave_up != 0);
    s_ev.attempts = attempts;
    power_event_logic_restore(&s_ev, now_ms());
}

// Guarda la hora de pared cada minuto para poder detectar, tras un reinicio,
// cuanto tiempo estuvo el ESP32 apagado.
static void persist_last_seen(void) {
    uint32_t now_unix = (uint32_t)time(NULL);
    if (now_unix < 1704067200) return;   // reloj sin sincronizar
    if (now_unix == s_last_seen_unix) return;
    s_last_seen_unix = now_unix;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, NVS_KEY_LAST_SEEN, now_unix);
    nvs_commit(h);
    nvs_close(h);
}

// Al arrancar: si el ESP32 estuvo apagado mucho mas de lo que dura su propio
// auto-reboot, fue por un corte. Si el servidor deberia estar encendido, a
// devolverlo a la vida.
static void check_boot_gap(void) {
    if (s_gap_checked) return;
    uint32_t now_unix = (uint32_t)time(NULL);
    if (now_unix < 1704067200) return;   // todavia sin SNTP
    s_gap_checked = true;
    if (s_last_seen_unix == 0 || now_unix <= s_last_seen_unix) return;

    uint32_t gap_s = now_unix - s_last_seen_unix;
    // El auto-reboot normal deja un hueco ~= su intervalo. Si el hueco es
    // mucho mayor, el ESP32 estuvo apagado por otra cosa (corte de luz).
    uint32_t interval_min = auto_reboot_get_interval_min();
    uint32_t threshold_s = (interval_min > 0 ? interval_min * 2 : 30U) * 60U + 600U;
    if (gap_s <= threshold_s) return;

    ESP_LOGW(TAG, "estuve apagado %u min (umbral %u min): posible corte",
             (unsigned)(gap_s / 60U), (unsigned)(threshold_s / 60U));
    power_event_logic_note_gap(&s_ev, gap_s * 1000U, now_ms());
    persist();
}

void power_event_note_net_down(void) {
    if (!s_initialized) return;
    uint32_t before = s_ev.phase;
    power_event_logic_note_net_down(&s_ev, now_ms());
    if (s_ev.phase != before) {
        ESP_LOGW(TAG, "red caida: posible corte (server_was_on=%d)",
                 (int)s_ev.server_was_on);
        persist();
    }
}

void power_event_note_net_up(void) {
    if (!s_initialized) return;
    uint32_t before = s_ev.phase;
    power_event_logic_note_net_up(&s_ev, now_ms());
    if (s_ev.phase != before) {
        ESP_LOGW(TAG, "red de vuelta tras %u ms -> fase %d (attempts=%u)",
                 (unsigned)s_ev.outage_ms, (int)s_ev.phase,
                 (unsigned)s_ev.attempts);
        persist();
    }
}

void power_event_note_server_on(void) {
    if (!s_initialized) return;
    if (!s_ev.server_was_on) {
        power_event_logic_note_server_on(&s_ev, now_ms());
        persist();
    }
}

void power_event_note_intentional_off(void) {
    if (!s_initialized) return;
    if (s_ev.server_was_on || s_ev.phase != POWER_EVENT_PHASE_NORMAL) {
        power_event_logic_note_intentional_off(&s_ev, now_ms());
        persist();
    }
}

void power_event_status_str(char *out, size_t out_size) {
    if (out == NULL || out_size == 0) return;
    const char *phase = "normal";
    switch (s_ev.phase) {
        case POWER_EVENT_PHASE_NET_DOWN:       phase = "red caida"; break;
        case POWER_EVENT_PHASE_WAITING_SERVER: phase = "esperando al servidor"; break;
        default: break;
    }
    snprintf(out, out_size,
             "power-event: %s (was_on=%d, intentos=%u, avisado=%d, rendido=%d)",
             phase, (int)s_ev.server_was_on, (unsigned)s_ev.attempts,
             (int)s_ev.notified, (int)s_ev.gave_up);
}

static void notify_not_back(uint32_t attempts, uint32_t outage_ms) {
    char msg[560];
    snprintf(msg, sizeof(msg),
        "CORTE DETECTADO: el servidor estaba encendido y no ha vuelto "
        "(red caida %u min).\n"
        "He enviado %u rafagas de magic packet y no responde: el ESP32 no "
        "puede despertarlo.\n"
        "Si sigue asi: enciendelo a mano con el boton del case y revisa la "
        "BIOS del servidor:\n"
        "  - AC BACK = Always On (que arranque solo al volver la luz)\n"
        "  - ErP = Disabled (que la fuente no se duerma del todo)\n"
        "  - Power On By PME/PCI-E = Enabled\n"
        "Con eso, el servidor vuelve solo y el ESP32 lo puede despertar.",
        (unsigned)(outage_ms / 60000U), (unsigned)attempts);
    ESP_LOGE(TAG, "%s", msg);
    telegram_broadcast(msg);
}

static void notify_back(uint32_t attempts) {
    char msg[240];
    snprintf(msg, sizeof(msg),
        "El servidor VOLVIO despues del corte (%u intentos de magic packet).",
        (unsigned)attempts);
    ESP_LOGI(TAG, "%s", msg);
    telegram_broadcast(msg);
}

static void power_event_task(void *arg) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        if (!s_initialized) continue;

        // Guardar la hora de pared (para detectar apagones largos al arrancar).
        if ((now_ms() % 60000UL) < POLL_MS) persist_last_seen();
        // Tras arrancar: si estuvimos apagados mucho mas de lo normal, fue un corte.
        check_boot_gap();

        // El servidor esta vivo si el ultimo poll del estado lo vio responder.
        bool alive = state_last_poll_tcp_alive();
        power_event_action_t act =
            power_event_logic_decide(&s_ev, now_ms(), alive);

        switch (act) {
            case POWER_EVENT_ACTION_WAKE: {
                ESP_LOGI(TAG, "intento %u de despertar al servidor tras el corte",
                         (unsigned)s_ev.attempts);
                // wake_request() abre un episodio de 4 min con rafagas cada 10 s;
                // re-armarlo cada 2 min es la escalada con esperas.
                wake_request("power-event");
                persist();
                break;
            }
            case POWER_EVENT_ACTION_NOTIFY:
                notify_not_back(s_ev.attempts, s_ev.outage_ms);
                persist();
                break;
            case POWER_EVENT_ACTION_NOTIFY_BACK:
                notify_back(s_ev.attempts);
                persist();
                break;
            default:
                break;
        }
    }
}

static void simulate_task(void *arg) {
    uint32_t sec = (uint32_t)(uintptr_t)arg;
    ESP_LOGW(TAG, "SIMULANDO corte de red de %u s", (unsigned)sec);
    power_event_logic_note_net_down(&s_ev, now_ms());
    persist();
    vTaskDelay(pdMS_TO_TICKS(sec * 1000U));
    ESP_LOGW(TAG, "fin de la simulacion: la red vuelve");
    power_event_logic_note_net_up(&s_ev, now_ms());
    persist();
    vTaskDelete(NULL);
}

void power_event_simulate_outage(uint32_t seconds) {
    if (seconds < 5) seconds = 5;
    if (seconds > 3600) seconds = 3600;
    xTaskCreate(simulate_task, "power_sim", 8192,
                (void *)(uintptr_t)seconds, 3, NULL);
}

esp_err_t power_event_init(void) {
    load();
    s_initialized = true;
    BaseType_t ok = xTaskCreate(power_event_task, "power_event", TASK_STACK,
                                NULL, TASK_PRIO, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(power_event) failed");
        s_initialized = false;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "vigilante de cortes listo (fase=%d, server_was_on=%d)",
             (int)s_ev.phase, (int)s_ev.server_was_on);
    return ESP_OK;
}
