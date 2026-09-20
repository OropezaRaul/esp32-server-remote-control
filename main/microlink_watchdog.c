#include "microlink_watchdog.h"

#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <stdio.h>

#include "microlink_wrapper.h"
#include "telegram.h"
#include "wd_logic.h"

#include "sdkconfig.h"

static const char *TAG = "ml_watchdog";

#if CONFIG_MICROLINK_WATCHDOG_ENABLED

#define WATCHDOG_STACK    3072
#define WATCHDOG_PRIO     3

/* Ventana de gracia (us) para el notify de Telegram ANTES del reset. Si el
 * POST a api.telegram.org se cuelga (bug histórico de esp_http_client en IDF
 * v5.3: timeouts ignorados en la fase de conexión cuando la red está muerta),
 * el timer dispara esp_restart() igual — el reboot NO puede quedar bloqueado
 * por una llamada de red. */
#define ML_WD_REBOOT_NOTIFY_GRACE_US (2000ULL * 1000ULL)

/* Callback del timer de deadline: camino GARANTIZADO de reset. Se ejecuta en
 * el contexto del task de esp_timer (task context, esp_restart() es válido). */
static void reboot_deadline_cb(void *arg) {
    ESP_LOGE(TAG, ">>> reboot deadline fired — esp_restart() incondicional");
    esp_restart();
}

typedef struct {
    bool ever_connected;
    microlink_wd_recovery_kind_t last_recovery_kind;
    uint32_t total_recoveries;      /* reboots completos */
    uint32_t total_rebinds;         /* rebinds suaves */
    uint32_t last_recovery_ms;      /* tick del ultimo REBOOT disparado */
    uint32_t last_recovery_target_s;
    uint32_t last_rebind_ms;        /* tick del ultimo REBIND disparado */
    bool rebind_done;               /* rebind ya intentado en este episodio */
    bool task_running;
} watchdog_state_t;

static watchdog_state_t s_wd = {
    .ever_connected = false,
    .last_recovery_kind = ML_WD_RECOVERY_NONE,
    .total_recoveries = 0,
    .total_rebinds = 0,
    .last_recovery_ms = 0,
    .last_recovery_target_s = 0,
    .last_rebind_ms = 0,
    .rebind_done = false,
    .task_running = false,
};

static uint32_t now_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void notify_telegram(const char *line) {
    if (line == NULL || *line == 0) return;
    esp_err_t err = telegram_broadcast(line);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "telegram notify failed: %s", esp_err_to_name(err));
    }
}

/* Notifica a Telegram y luego rebootea el ESP32. El REBOOT es GARANTIZADO:
 * se arma un timer de deadline (esp_timer) ANTES del notify, así un notify
 * colgado (red muerta) no puede impedir el esp_restart(). El notify es
 * best-effort: si vuelve antes de la gracia, rebooteamos directo. */
static void trigger_reboot(uint32_t target_s) {
    /* Rate-limit: si disparamos algo hace poco, no duplicar. */
    uint32_t tick = now_ms();
    if (s_wd.last_recovery_ms != 0 &&
        (tick - s_wd.last_recovery_ms) < ML_WD_RECOVERY_COOLDOWN_MS) {
        ESP_LOGW(TAG, "reboot skipped (cooldown, %u ms since last)",
                 (unsigned)(tick - s_wd.last_recovery_ms));
        return;
    }

    s_wd.last_recovery_kind = ML_WD_RECOVERY_REBOOT;
    s_wd.last_recovery_ms = now_ms();
    s_wd.last_recovery_target_s = target_s;
    s_wd.total_recoveries++;

    ESP_LOGE(TAG, ">>> REBOOT triggered after %u s disconnected", (unsigned)target_s);

    /* 1) Deadline incondicional: pase lo que pase con el notify, en
     * ML_WD_REBOOT_NOTIFY_GRACE_US ejecutamos esp_restart(). */
    const esp_timer_create_args_t timer_args = {
        .callback = reboot_deadline_cb,
        .name = "wd_reboot_deadline",
    };
    esp_timer_handle_t deadline = NULL;
    esp_err_t terr = esp_timer_create(&timer_args, &deadline);
    if (terr != ESP_OK || esp_timer_start_once(deadline, ML_WD_REBOOT_NOTIFY_GRACE_US) != ESP_OK) {
        ESP_LOGE(TAG, "deadline timer unavailable (%s) — rebooting directly",
                 esp_err_to_name(terr));
        esp_restart();
    }

    /* 2) Notify best-effort. Si se cuelga, el timer de arriba corta. Si
     * vuelve rápido, esp_restart() se ejecuta acá (el timer queda sin usar). */
    notify_telegram("Watchdog: Tailscale sin conexion > umbral. Reboot del ESP32 en curso.");

    /* Breve delay para que el POST de Telegram termine de enviarse antes
     * del reset (esp_restart no espera I/O pendiente). */
    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_LOGE(TAG, "calling esp_restart()");
    esp_restart();
    /* No retorna. La proxima linea que se ejecuta aqui es tras el boot. */
}

/* Llena un wd_input_t con el snapshot actual (fuente unica de verdad:
 * la misma funcion de decision corre en wd_evaluate() y en /diag). */
static void fill_wd_input(wd_input_t *in, microlink_state_t st) {
    memset(in, 0, sizeof(*in));
    in->ever_connected = s_wd.ever_connected;
    in->state_connected = (st == ML_STATE_CONNECTED);
    in->last_connected_ms = microlink_wrapper_get_last_connected_ms();
    in->ctrl_stale_ms = microlink_wrapper_get_ctrl_stale_ms();
    in->tick = now_ms();
    in->last_reboot_ms = s_wd.last_recovery_ms;
    in->last_rebind_ms = s_wd.last_rebind_ms;
    in->rebind_done = s_wd.rebind_done;
    in->reboot_after_ms = (uint32_t)CONFIG_MICROLINK_WATCHDOG_REBOOT_AFTER_S * 1000U;
    in->rebind_after_ms = (uint32_t)CONFIG_MICROLINK_WATCHDOG_REBIND_AFTER_S * 1000U;
    in->cooldown_ms = ML_WD_RECOVERY_COOLDOWN_MS;
}

static void watchdog_task(void *arg) {
    const uint32_t check_ms = (uint32_t)CONFIG_MICROLINK_WATCHDOG_CHECK_INTERVAL_S * 1000U;

    ESP_LOGI(TAG,
             "task started: check=%us rebind_after=%us reboot_after=%us",
             (unsigned)CONFIG_MICROLINK_WATCHDOG_CHECK_INTERVAL_S,
             (unsigned)CONFIG_MICROLINK_WATCHDOG_REBIND_AFTER_S,
             (unsigned)CONFIG_MICROLINK_WATCHDOG_REBOOT_AFTER_S);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(check_ms));

        microlink_state_t st = microlink_wrapper_get_state();
        uint32_t last_connected = microlink_wrapper_get_last_connected_ms();

        /* Armado: solo despues del primer CONNECTED. */
        if (!s_wd.ever_connected) {
            if (st == ML_STATE_CONNECTED && last_connected != 0) {
                s_wd.ever_connected = true;
                s_wd.last_recovery_kind = ML_WD_RECOVERY_NONE;
                s_wd.rebind_done = false;
                ESP_LOGI(TAG, "first CONNECTED observed -- watchdog armed");
            }
            continue;
        }

        wd_input_t in;
        fill_wd_input(&in, st);
        wd_action_t act = wd_evaluate(&in);

        /* Episodio terminado: hay evidencia de vida mas fresca que el
         * umbral de rebind (sesion recuperada por si sola o por el rebind
         * anterior). Permite un nuevo rebind en el proximo episodio. */
        if (in.disconnect_ms < in.rebind_after_ms) {
            if (s_wd.rebind_done) {
                ESP_LOGI(TAG, "session alive again (disconnect=%us) -- rebind window reset",
                         (unsigned)(in.disconnect_ms / 1000U));
            }
            s_wd.rebind_done = false;
        }

        switch (act) {
        case WD_ACT_REBOOT: {
            char msg[160];
            snprintf(msg, sizeof(msg),
                     "Watchdog: Tailscale sin vida por %u s (state=%s). Reboot del ESP32.",
                     (unsigned)(in.disconnect_ms / 1000U),
                     microlink_wrapper_state_name(st));
            ESP_LOGW(TAG, "%s", msg);
            trigger_reboot(CONFIG_MICROLINK_WATCHDOG_REBOOT_AFTER_S);
            /* No retorna (esp_restart), salvo cooldown. */
            break;
        }
        case WD_ACT_REBIND: {
            s_wd.rebind_done = true;
            s_wd.last_rebind_ms = now_ms();
            s_wd.total_rebinds++;
            s_wd.last_recovery_kind = ML_WD_RECOVERY_REBIND;
            char msg[160];
            snprintf(msg, sizeof(msg),
                     "Watchdog: Tailscale sin vida por %u s (state=%s%s). Rebind de la sesion...",
                     (unsigned)(in.disconnect_ms / 1000U),
                     microlink_wrapper_state_name(st),
                     in.zombie ? ", sesion zombie" : "");
            ESP_LOGW(TAG, ">>> %s", msg);
            notify_telegram(msg);
            esp_err_t rb = microlink_wrapper_rebind();
            if (rb != ESP_OK) {
                ESP_LOGE(TAG, "rebind failed: %s (reboot queda programado a los %u s)",
                         esp_err_to_name(rb),
                         (unsigned)CONFIG_MICROLINK_WATCHDOG_REBOOT_AFTER_S);
            }
            break;
        }
        case WD_ACT_PRE_WARN:
            ESP_LOGW(TAG, "disconnected %u s (state=%s%s; threshold reboot=%u s)",
                     (unsigned)(in.disconnect_ms / 1000U),
                     microlink_wrapper_state_name(st),
                     in.zombie ? ", ZOMBIE: control plane sin RX" : "",
                     (unsigned)CONFIG_MICROLINK_WATCHDOG_REBOOT_AFTER_S);
            break;
        case WD_ACT_NONE:
        default:
            if (in.disconnect_ms < in.rebind_after_ms &&
                s_wd.last_recovery_kind != ML_WD_RECOVERY_NONE) {
                ESP_LOGI(TAG,
                         "CONNECTED recovered (total reboots: %u, rebinds: %u)",
                         (unsigned)s_wd.total_recoveries,
                         (unsigned)s_wd.total_rebinds);
                s_wd.last_recovery_kind = ML_WD_RECOVERY_NONE;
            }
            break;
        }
    }
}

#else  /* CONFIG_MICROLINK_WATCHDOG_ENABLED */

static void watchdog_task(void *arg) {
    ESP_LOGW(TAG, "watchdog disabled via Kconfig");
    vTaskDelete(NULL);
}

#endif /* CONFIG_MICROLINK_WATCHDOG_ENABLED */

esp_err_t microlink_watchdog_init(void) {
#if CONFIG_MICROLINK_WATCHDOG_ENABLED
    if (s_wd.task_running) {
        ESP_LOGW(TAG, "watchdog already running");
        return ESP_OK;
    }
    BaseType_t ok = xTaskCreate(watchdog_task, "ml_watchdog",
                                WATCHDOG_STACK, NULL, WATCHDOG_PRIO, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed");
        return ESP_FAIL;
    }
    s_wd.task_running = true;
    ESP_LOGI(TAG, "init OK");
    return ESP_OK;
#else
    ESP_LOGI(TAG, "init skipped (CONFIG_MICROLINK_WATCHDOG_ENABLED=n)");
    return ESP_OK;
#endif
}

void microlink_watchdog_get_status(microlink_wd_status_t *out) {
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
#if CONFIG_MICROLINK_WATCHDOG_ENABLED
    out->enabled = true;
    out->ever_connected = s_wd.ever_connected;
    out->total_recoveries = s_wd.total_recoveries;
    out->total_rebinds = s_wd.total_rebinds;
    out->last_recovery_kind = s_wd.last_recovery_kind;

    uint32_t tick = now_ms();

    if (s_wd.last_recovery_ms != 0 && tick >= s_wd.last_recovery_ms) {
        out->last_recovery_ms_ago = tick - s_wd.last_recovery_ms;
    }
    out->last_recovery_target_s = s_wd.last_recovery_target_s;

    if (s_wd.last_rebind_ms != 0 && tick >= s_wd.last_rebind_ms) {
        out->last_rebind_ms_ago = tick - s_wd.last_rebind_ms;
    }

    /* Misma metrica y misma funcion de decision que la task: un solo
     * lugar donde vive la verdad sobre "cuanto lleva caido". */
    microlink_state_t st = microlink_wrapper_get_state();
    wd_input_t in;
    fill_wd_input(&in, st);
    (void)wd_evaluate(&in);
    out->disconnect_ms = in.disconnect_ms;
    out->ctrl_rx_ms_ago = in.ctrl_stale_ms;
#else
    out->enabled = false;
#endif
}

esp_err_t microlink_watchdog_force_recover(void) {
#if CONFIG_MICROLINK_WATCHDOG_ENABLED
    /* Disparar reboot inmediato. No-op si estamos en cooldown. */
    trigger_reboot(0);
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static const char *recovery_kind_name(microlink_wd_recovery_kind_t k) {
    switch (k) {
        case ML_WD_RECOVERY_NONE:   return "none";
        case ML_WD_RECOVERY_REBOOT: return "reboot";
        case ML_WD_RECOVERY_REBIND: return "rebind";
    }
    return "?";
}

void microlink_watchdog_format_status(char *out, size_t out_size) {
    if (out == NULL || out_size == 0) return;
    microlink_wd_status_t st;
    microlink_watchdog_get_status(&st);

#if CONFIG_MICROLINK_WATCHDOG_ENABLED
    char disconnect[32] = "0";
    if (st.disconnect_ms != 0) {
        snprintf(disconnect, sizeof(disconnect), "%u",
                 (unsigned)(st.disconnect_ms / 1000U));
    }
    char ctrl_rx[32] = "never";
    if (st.ctrl_rx_ms_ago != 0) {
        snprintf(ctrl_rx, sizeof(ctrl_rx), "%us ago",
                 (unsigned)(st.ctrl_rx_ms_ago / 1000U));
    }
    char last_rec[32] = "never";
    if (st.last_recovery_ms_ago != 0) {
        snprintf(last_rec, sizeof(last_rec), "%us ago (%s)",
                 (unsigned)(st.last_recovery_ms_ago / 1000U),
                 recovery_kind_name(st.last_recovery_kind));
    }
    char last_reb[32] = "never";
    if (st.last_rebind_ms_ago != 0) {
        snprintf(last_reb, sizeof(last_reb), "%us ago",
                 (unsigned)(st.last_rebind_ms_ago / 1000U));
    }
    snprintf(out, out_size,
             "Tailscale watchdog: %s\n"
             "  ever connected:    %s\n"
             "  current state:     %s\n"
             "  disconnected for:  %ss\n"
             "  control rx:        %s\n"
             "  total reboots:     %u\n"
             "  last reboot:       %s\n"
             "  total rebinds:     %u\n"
             "  last rebind:       %s\n"
             "  thresholds:        rebind>%us -> reboot>%us",
             st.enabled ? "ENABLED" : "DISABLED",
             st.ever_connected ? "yes" : "no",
             microlink_wrapper_state_name(microlink_wrapper_get_state()),
             disconnect,
             ctrl_rx,
             (unsigned)st.total_recoveries,
             last_rec,
             (unsigned)st.total_rebinds,
             last_reb,
             (unsigned)CONFIG_MICROLINK_WATCHDOG_REBIND_AFTER_S,
             (unsigned)CONFIG_MICROLINK_WATCHDOG_REBOOT_AFTER_S);
#else
    snprintf(out, out_size, "Tailscale watchdog: DISABLED (Kconfig)");
#endif
}
