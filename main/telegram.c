#include "telegram.h"

#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_tls.h>
#include <esp_crt_bundle.h>
#include <esp_clk_tree.h>
#include <soc/clk_tree_defs.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <inttypes.h>
#include <nvs.h>
#include <nvs_flash.h>

#include "wifi.h"
#include "state.h"
#include "wol.h"
#include "cocopa01_client.h"
#include "automation.h"
#include "scheduler.h"
#include "auto_reboot.h"
#include "portmap.h"
#include "microlink_wrapper.h"
#include "microlink_watchdog.h"
#include "version.h"
#include "system_health.h"
#include <esp_psram.h>

#include "sdkconfig.h"

static const char *TAG = "telegram";

#define MAX_AUTH_IDS        16
#define TG_POLL_INTERVAL_MS 500
#define TG_POLL_TIMEOUT_S   5
#define TG_HTTP_TIMEOUT_MS  8000

// Tras N polls exitosos consecutivos recreamos el cliente de polling desde
// cero. El socket TLS persistente a api.telegram.org puede quedar en estado
// half-open tras horas (idle close del edge / NAT) y el siguiente perform()
// con keep_alive_enable=true se CUELGA sin retornar error detectable
// durante >30s, bloqueando el long-poll y perdiendo todos los updates que
// Telegram tenía en cola. Recrear el cliente cada N ciclos cierra ese
// vector. N=60 ≈ 30s a TG_POLL_INTERVAL_MS=500ms.
#define TG_POLL_RECREATE_AFTER 60

#define TG_API_HOST "api.telegram.org"

static int64_t g_auth_ids[MAX_AUTH_IDS];
static int g_auth_ids_count = 0;
static int64_t g_last_update_id = 0;

static int parse_auth_ids(const char *raw) {
    if (raw == NULL || *raw == 0) return 0;
    int count = 0;
    char buf[256];
    strncpy(buf, raw, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    char *save = NULL;
    char *tok = strtok_r(buf, ",", &save);
    while (tok != NULL && count < MAX_AUTH_IDS) {
        while (*tok && (*tok == ' ' || *tok == '\t')) tok++;
        size_t len = strlen(tok);
        while (len > 0 && (tok[len - 1] == ' ' || tok[len - 1] == '\t' || tok[len - 1] == '\r' || tok[len - 1] == '\n')) {
            tok[--len] = 0;
        }
        if (len > 0) {
            g_auth_ids[count++] = strtoll(tok, NULL, 10);
        }
        tok = strtok_r(NULL, ",", &save);
    }
    return count;
}

static bool is_authorized(int64_t chat_id) {
    for (int i = 0; i < g_auth_ids_count; i++) {
        if (g_auth_ids[i] == chat_id) return true;
    }
    return false;
}

static void tg_url(char *out, size_t out_size, const char *path_and_query) {
    snprintf(out, out_size, "https://%s/bot%s/%s", TG_API_HOST, CONFIG_TELEGRAM_BOT_TOKEN, path_and_query);
}

static const char *cmd_name(const char *text, size_t *out_len) {
    if (text == NULL) return NULL;
    const char *p = text;
    while (*p == '/' || *p == '@') p++;
    if (*p == 0) return NULL;
    const char *end = p;
    while (*end && *end != ' ' && *end != '\t' && *end != '\n' && *end != '\r' && *end != '@') end++;
    *out_len = end - p;
    return p;
}

// /schedule            -> reporta wake/off programados
// /schedule set wake HH:MM -> wake diario a HH:MM (Perú UTC-5)
// /schedule set off  HH:MM -> /off diario a HH:MM
// /schedule reset      -> borra wake+off
static void handle_schedule_cmd(int64_t chat_id, const char *args) {
    if (args == NULL || *args == 0) {
        char buf[SCHEDULER_STATUS_BUF_SIZE];
        scheduler_status_str(buf, sizeof(buf));
        telegram_send_text(chat_id, buf);
        return;
    }

    char word1[16] = {0}, word2[16] = {0};
    const char *p = args;
    int n = 0;
    while (*p && !isspace((unsigned char)*p) && n < (int)sizeof(word1) - 1) {
        word1[n++] = *p++;
    }
    word1[n] = 0;
    while (*p == ' ' || *p == '\t') p++;
    n = 0;
    while (*p && !isspace((unsigned char)*p) && n < (int)sizeof(word2) - 1) {
        word2[n++] = *p++;
    }
    word2[n] = 0;
    while (*p == ' ' || *p == '\t') p++;

    if (strcmp(word1, "reset") == 0) {
        if (scheduler_reset() != ESP_OK) {
            telegram_send_text(chat_id, "ERR: failed to clear schedule (NVS)");
            return;
        }
        telegram_send_text(chat_id, "OK: schedule cleared. No scheduled wake/off.");
        return;
    }

    if (strcmp(word1, "set") == 0) {
        bool is_wake = (strcmp(word2, "wake") == 0);
        bool is_off  = (strcmp(word2, "off") == 0);
        if (!is_wake && !is_off) {
            telegram_send_text(chat_id, "ERR: usage /schedule set wake|off HH:MM");
            return;
        }
        int mins = automation_parse_hhmm(p);
        if (mins < 0) {
            telegram_send_text(chat_id,
                "ERR: invalid time. Expected 24h HH:MM (e.g. 08:00).");
            return;
        }
        scheduler_config_t cfg;
        scheduler_get(&cfg);
        if (is_wake) {
            cfg.wake_set = true;
            cfg.wake_min = (uint16_t)mins;
        } else {
            cfg.off_set  = true;
            cfg.off_min  = (uint16_t)mins;
        }
        if (scheduler_set(&cfg) != ESP_OK) {
            telegram_send_text(chat_id, "ERR: failed to save schedule (NVS)");
            return;
        }
        char buf[SCHEDULER_STATUS_BUF_SIZE];
        scheduler_status_str(buf, sizeof(buf));
        char ack[SCHEDULER_STATUS_BUF_SIZE + 32];
        snprintf(ack, sizeof(ack), "OK: saved.\n%s", buf);
        telegram_send_text(chat_id, ack);
        return;
    }

    telegram_send_text(chat_id,
        "Schedule usage:\n"
        "/schedule - show scheduled wake/off\n"
        "/schedule set wake HH:MM - daily WOL at HH:MM\n"
        "/schedule set off HH:MM - daily /off at HH:MM\n"
        "/schedule reset - clear scheduled wake/off\n"
        "24h format, hora Perú (UTC-5). Persists in NVS.");
}

// /reboot               -> reinicia el ESP32 inmediatamente
// /reboot now           -> reinicia el ESP32 inmediatamente
// /reboot <min>         -> configura intervalo de auto-reboot en minutos (persiste en NVS)
// /reboot 0 (o off)     -> deshabilita auto-reboot (switch off)
// /reboot status (info) -> muestra estado, intervalo, uptime y tiempo restante
// /reboot help          -> ayuda
static void handle_reboot_cmd(int64_t chat_id, const char *args) {
    char clean[64] = {0};
    if (args != NULL) {
        while (*args == ' ' || *args == '\t') args++;
        size_t len = strlen(args);
        while (len > 0 && (args[len - 1] == ' ' || args[len - 1] == '\t' || args[len - 1] == '\r' || args[len - 1] == '\n')) {
            len--;
        }
        if (len >= sizeof(clean)) len = sizeof(clean) - 1;
        memcpy(clean, args, len);
        clean[len] = 0;
    }

    if (clean[0] == 0 || strcmp(clean, "now") == 0) {
        auto_reboot_trigger("telegram /reboot", chat_id);
        return;
    }

    if (strcmp(clean, "help") == 0) {
        telegram_send_text(chat_id,
            "Auto-reboot commands:\n"
            "/reboot - Reboot ESP32 immediately (also: /reboot now)\n"
            "/reboot <min> - Set auto-reboot interval in minutes (persists in NVS)\n"
            "/reboot 0 - Disable auto-reboot (switch off)\n"
            "/reboot status - View auto-reboot status and remaining time\n"
            "/reboot help - Show this help\n\n"
            "Default: 60 minutes (1 hour). Setting 0 turns off auto-reboot.");
        return;
    }

    if (strcmp(clean, "status") == 0 || strcmp(clean, "info") == 0) {
        char buf[AUTO_REBOOT_STATUS_BUF_SIZE];
        auto_reboot_format_status(buf, sizeof(buf));
        telegram_send_text(chat_id, buf);
        return;
    }

    if (strcasecmp(clean, "off") == 0 || strcasecmp(clean, "disable") == 0 || strcasecmp(clean, "disabled") == 0) {
        if (auto_reboot_set_interval_min(0) == ESP_OK) {
            telegram_send_text(chat_id, "OK: Auto-reboot disabled (switch off). ESP32 will not reboot automatically.");
        } else {
            telegram_send_text(chat_id, "ERR: failed to save to NVS");
        }
        return;
    }

    char *endptr = NULL;
    long val = strtol(clean, &endptr, 10);
    if (endptr != clean) {
        while (*endptr == ' ' || *endptr == '\t') endptr++;
        if (*endptr == 'h' || *endptr == 'H') {
            val *= 60;
            endptr++;
            while (*endptr == ' ' || *endptr == '\t') endptr++;
        } else if (*endptr == 'm' || *endptr == 'M') {
            endptr++;
            if (*endptr == 'i' || *endptr == 'I') endptr++;
            if (*endptr == 'n' || *endptr == 'N') endptr++;
            while (*endptr == ' ' || *endptr == '\t') endptr++;
        }

        if (*endptr == 0) {
            if (val < 0 || val > 10080) {
                telegram_send_text(chat_id, "ERR: interval must be 0 (disable) or 1..10080 minutes (max 7 days).");
                return;
            }
            if (val == 0) {
                if (auto_reboot_set_interval_min(0) == ESP_OK) {
                    telegram_send_text(chat_id, "OK: Auto-reboot disabled (switch off). ESP32 will not reboot automatically.");
                } else {
                    telegram_send_text(chat_id, "ERR: failed to save to NVS");
                }
                return;
            }

            if (auto_reboot_set_interval_min((uint32_t)val) == ESP_OK) {
                char buf[128];
                snprintf(buf, sizeof(buf),
                         "OK: Auto-reboot interval set to %ld min (persists in NVS).\n"
                         "Next reboot in %ld min.",
                         val, val);
                telegram_send_text(chat_id, buf);
            } else {
                telegram_send_text(chat_id, "ERR: failed to save to NVS");
            }
            return;
        }
    }

    telegram_send_text(chat_id,
        "ERR: invalid argument. Use /reboot <min>, /reboot 0, /reboot now, or /reboot help.");
}


static void handle_command(int64_t chat_id, const char *cmd, size_t cmd_len, const char *args) {
    if (cmd_len == 4 && strncmp(cmd, "wake", 4) == 0) {
        if (wol_send_default() == ESP_OK) {
            state_mark_known_on();
            telegram_send_text(chat_id, "OK: Magic packet sent to cocopa01.");
        } else {
            telegram_send_text(chat_id, "Cooldown active. Wait before retry.");
        }
    } else if (cmd_len == 6 && strncmp(cmd, "status", 6) == 0) {
        // /status responde rápido (sin TCP bloqueante) usando el cache
        // que mantiene state_poll_task. El TCP en vivo está disponible
        // vía /ping. Aquí solo lectura de variables volatiles + un par
        // de heap_caps_get_free_size (O(num_bloques_heap)) = microsegundos.
        cocopa01_state_t st = state_get();
        uint32_t in_ms = state_entered_at_ms();
        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        uint32_t elapsed = (in_ms > 0) ? (now_ms - in_ms) : 0;
        uint32_t hours = elapsed / 3600000UL;
        uint32_t mins  = (elapsed % 3600000UL) / 60000UL;
        uint32_t up_s = state_cocopa01_uptime_s();
        uint32_t poll_age_ms = state_last_poll_ms();
        uint32_t poll_age_s = (poll_age_ms > 0 && now_ms > poll_age_ms)
            ? (now_ms - poll_age_ms) / 1000UL
            : 0;
        const char *icon = "UNKNOWN";
        switch (st) {
            case COCOPA01_STATE_ON:      icon = "ON";      break;
            case COCOPA01_STATE_SUSPEND: icon = "SUSPEND"; break;
            case COCOPA01_STATE_OFF:     icon = "OFF";     break;
            default: break;
        }
        bool alive_recent = state_last_poll_tcp_alive();
        size_t heap_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t heap_internal_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        char poll_str[24];
        if (poll_age_ms == 0) {
            snprintf(poll_str, sizeof(poll_str), "never");
        } else if (poll_age_s < 60) {
            snprintf(poll_str, sizeof(poll_str), "%lus ago", (unsigned long)poll_age_s);
        } else {
            snprintf(poll_str, sizeof(poll_str), "%lum ago",
                     (unsigned long)(poll_age_s / 60));
        }
        char age_str[24];
        if (hours > 0) {
            snprintf(age_str, sizeof(age_str), "%luh %lum",
                     (unsigned long)hours, (unsigned long)mins);
        } else {
            snprintf(age_str, sizeof(age_str), "%lum", (unsigned long)mins);
        }
        char buf[512];
        if (up_s != 0) {
            uint32_t up_h = up_s / 3600;
            uint32_t up_m = (up_s % 3600) / 60;
            snprintf(buf, sizeof(buf),
                     "%s\n"
                     "IP: %s  State: %s (%s)\n"
                     "Uptime: %luh %lum  TCP/22: %s (poll %s)\n"
                     "Heap: %u free / %u min-ever  PSRAM: %u KB",
                     FIRMWARE_VERSION,
                     CONFIG_COCOPA01_IP, icon, age_str,
                     (unsigned long)up_h, (unsigned long)up_m,
                     alive_recent ? "UP" : "DOWN", poll_str,
                     (unsigned)heap_internal, (unsigned)heap_internal_min,
                     (unsigned)(psram_free / 1024));
        } else {
            snprintf(buf, sizeof(buf),
                     "%s\n"
                     "IP: %s  State: %s (%s)\n"
                     "TCP/22: %s (poll %s)\n"
                     "Heap: %u free / %u min-ever  PSRAM: %u KB",
                     FIRMWARE_VERSION,
                     CONFIG_COCOPA01_IP, icon, age_str,
                     alive_recent ? "UP" : "DOWN", poll_str,
                     (unsigned)heap_internal, (unsigned)heap_internal_min,
                     (unsigned)(psram_free / 1024));
        }
        telegram_send_text(chat_id, buf);
    } else if (cmd_len == 5 && strncmp(cmd, "stats", 5) == 0) {
        cocopa01_state_t st = state_get();
        if (st != COCOPA01_STATE_ON) {
            telegram_send_text(chat_id,
                "cocopa01 está en estado OFF/SUSPEND. Enciende el servidor con /wake para consultar estadísticas.");
            return;
        }
        char *buf = malloc(2048);
        if (buf == NULL) {
            telegram_send_text(chat_id, "ERR: out of memory allocating stats buffer");
            return;
        }
        esp_err_t err = cocopa01_get_stats(buf, 2048);
        if (err == ESP_OK) {
            telegram_send_text(chat_id, buf);
        } else if (err == ESP_ERR_TIMEOUT) {
            telegram_send_text(chat_id, "ERR: timeout conectando al monitor de cocopa01 (:8085)");
        } else {
            telegram_send_text(chat_id, "ERR: cocopa01 está encendido pero el monitor no responde en puerto 8085");
        }
        free(buf);
    } else if (cmd_len == 4 && strncmp(cmd, "ping", 4) == 0) {
        // /ping: TCP live a cocopa01:22 (bloquea hasta el timeout, pero el
        // usuario pidió explícitamente una probe fresca).
        uint32_t t0 = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        bool ok = cocopa01_tcp_alive(2000);
        uint32_t dt = ((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) - t0);
        char buf[96];
        snprintf(buf, sizeof(buf), "ping %s:%d -> %s (%lu ms)",
                 CONFIG_COCOPA01_IP, CONFIG_COCOPA01_SSH_PORT,
                 ok ? "UP" : "DOWN", (unsigned long)dt);
        telegram_send_text(chat_id, buf);
    } else if (cmd_len == 4 && strncmp(cmd, "diag", 4) == 0) {
        // /diag: heap/sockets/Uptime del ESP32, sin tocar cocopa01.
        // Útil para diagnosticar "por qué /status tarda" sin gastar TCP.
        size_t h_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t h_int_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        size_t h_int_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        size_t h_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        UBaseType_t n_tasks = uxTaskGetNumberOfTasks();
        char wd_buf[400];
        microlink_watchdog_format_status(wd_buf, sizeof(wd_buf));
        char ts_state[32];
        snprintf(ts_state, sizeof(ts_state), "%s",
                 microlink_wrapper_state_name(microlink_wrapper_get_state()));
        uint32_t vpn_ip = microlink_wrapper_get_vpn_ip();
        char vpn_str[20] = "none";
        if (vpn_ip) {
            snprintf(vpn_str, sizeof(vpn_str), "%lu.%lu.%lu.%lu",
                     (unsigned long)((vpn_ip >> 24) & 0xFF),
                     (unsigned long)((vpn_ip >> 16) & 0xFF),
                     (unsigned long)((vpn_ip >> 8) & 0xFF),
                     (unsigned long)(vpn_ip & 0xFF));
        }
        char ar_str[48];
        if (auto_reboot_is_enabled()) {
            uint32_t rem_m = auto_reboot_seconds_remaining() / 60;
            snprintf(ar_str, sizeof(ar_str), "%lu min (in %lu min)",
                     (unsigned long)auto_reboot_get_interval_min(),
                     (unsigned long)rem_m);
        } else {
            snprintf(ar_str, sizeof(ar_str), "disabled");
        }
        char health_buf[512];
        system_health_format_status(health_buf, sizeof(health_buf));
        char buf[1400];
        snprintf(buf, sizeof(buf),
                 "diag\n"
                 "Heap int: %u free / %u min / %u largest\n"
                 "PSRAM: %u KB free\n"
                 "Tasks: %u\n"
                 "Wi-Fi: %s\n"
                 "Tailscale: %s vpn=%s\n"
                 "Auto-reboot: %s\n"
                 "%s\n"
                 "%s",
                 (unsigned)h_int, (unsigned)h_int_min, (unsigned)h_int_largest,
                 (unsigned)(h_psram / 1024),
                 (unsigned)n_tasks,
                 wifi_is_connected() ? "connected" : "down",
                 ts_state, vpn_str,
                 ar_str,
                 wd_buf,
                 health_buf);
        telegram_send_text(chat_id, buf);
    } else if (cmd_len == 4 && strncmp(cmd, "info", 4) == 0) {
        char buf[512];
        uint32_t cpu_freq_hz = 0;
        esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_CPU, ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &cpu_freq_hz);
        snprintf(buf, sizeof(buf),
                 "%s\n"
                 "Chip: ESP32-S3 @ %" PRIu32 " MHz\n"
                 "PSRAM: %u KB free\n"
                 "Heap: %u bytes free\n"
                 "Wi-Fi: %s",
                 FIRMWARE_VERSION,
                 cpu_freq_hz / 1000000U,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 wifi_is_connected() ? "connected" : "down");
        telegram_send_text(chat_id, buf);
} else if (cmd_len == 3 && strncmp(cmd, "off", 3) == 0) {
    cocopa01_state_t cur = state_get();
    if (cur == COCOPA01_STATE_SUSPEND) {
        // cocopa01 is in S3; the NIC is down so /off would fail to
        // reach it. ACPI doesn't allow S3 -> S5 directly, so we have
        // to go S3 -> S0 (via WOL) -> S5. Do the whole sequence in one
        // /off invocation so the user only has to type one command.
        telegram_send_text(chat_id, "cocopa01 is in S3. Waking...");
        if (wol_send_default() != ESP_OK) {
            telegram_send_text(chat_id, "ERR: WOL send failed (cooldown?). Try again in a few seconds.");
            return;
        }
        // Wait for cocopa01 to come back online. Resume from S3 is fast
        // (~5-10s) but allow up to 60s for BIOS + userspace.
        bool came_up = false;
        for (int i = 0; i < 30; i++) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            if (cocopa01_tcp_alive(2000)) {
                came_up = true;
                break;
            }
        }
        if (!came_up) {
            state_set(COCOPA01_STATE_OFF);
            telegram_send_text(chat_id, "WARN: cocopa01 did not respond within 60s after WOL. Assuming already OFF.");
            return;
        }
        // cocopa01 is now in S0; send the shutdown command.
        if (cocopa01_send_off() == ESP_OK) {
            state_set(COCOPA01_STATE_OFF);
            telegram_send_text(chat_id, "OK: woken from S3 and shutdown sent.");
        } else {
            telegram_send_text(chat_id, "ERR: cocopa01 came back online but refused /off.");
        }
        return;
    }
    if (cocopa01_tcp_alive(2000)) {
        if (cocopa01_send_off() == ESP_OK) {
            state_set(COCOPA01_STATE_OFF);
            telegram_send_text(chat_id, "OK: cocopa01 shutdown sent.");
        } else {
            telegram_send_text(chat_id, "ERR: cocopa01 reachable but refused shutdown.");
        }
    } else {
        // TCP not alive AND state is not SUSPEND (we handled that above),
        // so cocopa01 is genuinely OFF or state is UNKNOWN. Trust the
        // recent polling result; don't flip to OFF blindly.
        if (cur == COCOPA01_STATE_OFF) {
            telegram_send_text(chat_id, "OK: cocopa01 already off.");
        } else {
            telegram_send_text(chat_id, "OK: cocopa01 not reachable (state UNKNOWN); assuming off.");
            state_set(COCOPA01_STATE_OFF);
        }
    }
    } else if (cmd_len == 7 && strncmp(cmd, "suspend", 7) == 0) {
        if (!cocopa01_tcp_alive(2000)) {
            telegram_send_text(chat_id, "ERR: cocopa01 not reachable. /suspend requires the host to be ON.");
            return;
        }
        if (cocopa01_send_suspend() == ESP_OK) {
            state_set(COCOPA01_STATE_SUSPEND);
            telegram_send_text(chat_id, "OK: cocopa01 suspend sent.");
        } else {
            telegram_send_text(chat_id, "ERR: cocopa01 reachable but refused suspend.");
        }
    } else if (cmd_len == 8 && strncmp(cmd, "schedule", 8) == 0) {
        handle_schedule_cmd(chat_id, args);
    } else if (cmd_len == 6 && strncmp(cmd, "reboot", 6) == 0) {
        handle_reboot_cmd(chat_id, args);
        char buf[512];
        int p = snprintf(buf, sizeof(buf), "Authorized chat IDs (%d):\n", g_auth_ids_count);
        for (int i = 0; i < g_auth_ids_count && p < (int)sizeof(buf) - 32; i++) {
            p += snprintf(buf + p, sizeof(buf) - p, "  %d. %lld\n", i + 1, (long long)g_auth_ids[i]);
        }
        telegram_send_text(chat_id, buf);
    } else if (cmd_len == 10 && strncmp(cmd, "automation", 10) == 0) {
        char buf[AUTOMATION_STATUS_BUF_SIZE];
        automation_status_str(buf, sizeof(buf));
        telegram_send_text(chat_id, buf);
    } else if (cmd_len == 7 && strncmp(cmd, "autoset", 7) == 0) {
        // /autoset <key> <val> [<key> <val> ...]
        //   enabled 0|1
        //   suspend <min>            (minutos de inactividad hasta suspend)
        //   off <min>                (minutos desde suspend hasta off; 0 deshabilita)
        //   quiet HH:MM-HH:MM        (rango de horario protegido, wrap-around OK)
        //   defaults                 (resetea a defaults de Kconfig)
        if (args == NULL || *args == 0) {
            telegram_send_text(chat_id,
                "Usage:\n"
                "/autoset enabled 0|1\n"
                "/autoset suspend <min>\n"
                "/autoset off <min>\n"
                "/autoset quiet HH:MM-HH:MM\n"
                "/autoset defaults\n"
                "Multiple pairs in one command:\n"
                "/autoset suspend 20 off 15");
            return;
        }

        // "defaults" no toma pares adicionales
        char first[32] = {0};
        int fn = 0;
        while (args[fn] && !isspace((unsigned char)args[fn]) && fn < (int)sizeof(first) - 1) {
            first[fn] = args[fn]; fn++;
        }
        first[fn] = 0;
        if (strcmp(first, "defaults") == 0) {
            if (automation_reset_defaults() != ESP_OK) {
                telegram_send_text(chat_id, "ERR: failed to save defaults");
                return;
            }
            telegram_send_text(chat_id, "OK: automation reset to defaults.");
            return;
        }

        automation_config_t cfg;
        automation_get_config(&cfg);

        const char *p = args;
        int pairs = 0;
        while (*p) {
            while (*p == ' ' || *p == '\t') p++;
            if (*p == 0) break;

            char key[32] = {0};
            int kn = 0;
            while (*p && !isspace((unsigned char)*p) && kn < (int)sizeof(key) - 1) {
                key[kn++] = *p++;
            }
            key[kn] = 0;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == 0) {
                char err[96];
                snprintf(err, sizeof(err), "ERR: key '%s' has no value", key);
                telegram_send_text(chat_id, err);
                return;
            }
            char val[64] = {0};
            int vn = 0;
            while (*p && !isspace((unsigned char)*p) && vn < (int)sizeof(val) - 1) {
                val[vn++] = *p++;
            }
            val[vn] = 0;

            if (strcmp(key, "enabled") == 0) {
                if (strcmp(val, "1") == 0 || strcmp(val, "true") == 0 || strcmp(val, "on") == 0) {
                    cfg.enabled = true;
                } else if (strcmp(val, "0") == 0 || strcmp(val, "false") == 0 || strcmp(val, "off") == 0) {
                    cfg.enabled = false;
                } else {
                    telegram_send_text(chat_id, "ERR: enabled expects 0 or 1");
                    return;
                }
            } else if (strcmp(key, "suspend") == 0) {
                int v = atoi(val);
                if (v < 1 || v > 1440) {
                    telegram_send_text(chat_id, "ERR: suspend expects 1..1440 minutes");
                    return;
                }
                cfg.suspend_after_min = (uint16_t)v;
            } else if (strcmp(key, "off") == 0) {
                int v = atoi(val);
                if (v < 0 || v > 1440) {
                    telegram_send_text(chat_id, "ERR: off expects 0..1440 minutes (0 disables)");
                    return;
                }
                cfg.off_after_min = (uint16_t)v;
            } else if (strcmp(key, "quiet") == 0) {
                // Formato: HH:MM-HH:MM
                char tmp[64];
                strncpy(tmp, val, sizeof(tmp) - 1);
                tmp[sizeof(tmp) - 1] = 0;
                char *dash = strchr(tmp, '-');
                if (dash == NULL) {
                    telegram_send_text(chat_id, "ERR: quiet expects HH:MM-HH:MM");
                    return;
                }
                *dash = 0;
                int qs = automation_parse_hhmm(tmp);
                int qe = automation_parse_hhmm(dash + 1);
                if (qs < 0 || qe < 0) {
                    telegram_send_text(chat_id, "ERR: invalid HH:MM in quiet range");
                    return;
                }
                cfg.quiet_start_min = (uint16_t)qs;
                cfg.quiet_end_min   = (uint16_t)qe;
            } else {
                char err[128];
                snprintf(err, sizeof(err),
                    "ERR: unknown key '%s'. Use: enabled, suspend, off, quiet, defaults",
                    key);
                telegram_send_text(chat_id, err);
                return;
            }
            pairs++;
        }

        if (pairs == 0) {
            telegram_send_text(chat_id, "ERR: no valid pairs");
            return;
        }

        if (automation_set_config(&cfg) != ESP_OK) {
            telegram_send_text(chat_id, "ERR: failed to save to NVS");
            return;
        }
        char confirm[AUTOMATION_STATUS_BUF_SIZE];
        automation_status_str(confirm, sizeof(confirm));
        char ack[AUTOMATION_STATUS_BUF_SIZE + 32];
        snprintf(ack, sizeof(ack), "OK: saved (%d pair%s).\n%s",
                 pairs, pairs == 1 ? "" : "s", confirm);
        telegram_send_text(chat_id, ack);
    } else if (cmd_len == 7 && strncmp(cmd, "portmap", 7) == 0) {
        // /portmap list | add <local> <upstream> [host] [proto] [label]
        //            | del <local> | defaults
        if (args == NULL || *args == 0) {
            telegram_send_text(chat_id,
                "Port mappings. Usage:\n"
                "/portmap list\n"
                "/portmap add <local> <upstream> [host] [proto] [label]\n"
                "/portmap del <local>\n"
                "/portmap defaults\n"
                "\n"
                "host empty = auto-route (LAN vs Tailscale).\n"
                "proto = http (default) or https.\n"
                "label = free text for identification.\n"
                "local >= 1024 (no privileged).");
            return;
        }

        char scratch[256];
        strncpy(scratch, args, sizeof(scratch) - 1);
        scratch[sizeof(scratch) - 1] = 0;
        char *save = NULL;
        char *subcmd = strtok_r(scratch, " \t", &save);

        if (subcmd == NULL) {
            telegram_send_text(chat_id, "ERR: missing subcommand. Use /portmap.");
            return;
        }

        if (strcmp(subcmd, "list") == 0) {
            char buf[PORTMAP_STATUS_BUF_SIZE];
            portmap_format_status(buf, sizeof(buf));
            telegram_send_text(chat_id, buf);
        } else if (strcmp(subcmd, "defaults") == 0) {
            if (portmap_reset_defaults() == ESP_OK) {
                telegram_send_text(chat_id, "OK: portmap reset to defaults (8080 -> 8080 http cocopa).");
            } else {
                telegram_send_text(chat_id, "ERR: failed to reset portmap");
            }
        } else if (strcmp(subcmd, "del") == 0) {
            char *tok_local = strtok_r(NULL, " \t", &save);
            if (!tok_local) {
                telegram_send_text(chat_id, "ERR: usage: /portmap del <local_port>");
                return;
            }
            long lp = strtol(tok_local, NULL, 10);
            if (lp < 1 || lp > 65535) {
                char eb[80];
                snprintf(eb, sizeof(eb), "ERR: local_port must be 1..65535 (got %ld)", lp);
                telegram_send_text(chat_id, eb);
                return;
            }
            esp_err_t err = portmap_del((uint16_t)lp);
            if (err == ESP_OK) {
                char ack[80];
                snprintf(ack, sizeof(ack), "OK: removed mapping :%ld", lp);
                telegram_send_text(chat_id, ack);
            } else if (err == ESP_ERR_NOT_FOUND) {
                char eb[80];
                snprintf(eb, sizeof(eb), "ERR: no mapping for :%ld", lp);
                telegram_send_text(chat_id, eb);
            } else {
                char eb[80];
                snprintf(eb, sizeof(eb), "ERR: delete failed (%s)", esp_err_to_name(err));
                telegram_send_text(chat_id, eb);
            }
        } else if (strcmp(subcmd, "add") == 0) {
            char *tok_local = strtok_r(NULL, " \t", &save);
            char *tok_up    = strtok_r(NULL, " \t", &save);

            if (!tok_local || !tok_up) {
                telegram_send_text(chat_id,
                    "ERR: usage: /portmap add <local> <upstream> [host] [proto] [label]\n"
                    "Note: 'host' must look like a hostname/IP (contain a dot or be numeric).\n"
                    "Use empty quotes '' for auto-routed host: /portmap add 8443 8443 '' https label");
                return;
            }

            // Optional args. Smart positional parsing to avoid the trap of
            // /portmap add 8443 8443 citas interpreting "citas" as the host.
            // Detect: an arg that looks like a hostname (has '.' or is all digits)
            // OR matches "http"/"https" → positional. Otherwise → it's the label
            // (and any remaining args are proto/host in reverse).
            char *tok_a = strtok_r(NULL, " \t", &save);  // first optional
            char *tok_b = strtok_r(NULL, " \t", &save);  // second optional
            char *tok_c = strtok_r(NULL, " \t", &save);  // third optional

            const char *host  = "";
            const char *proto = "http";
            const char *label = "";

            // Helpers (macros inlines for the simple predicates).
            int looks_like_host = (tok_a && (*tok_a != 0) && strchr(tok_a, '.') != NULL);
            int is_proto        = (tok_a && (*tok_a != 0)
                                  && (strcmp(tok_a, "http") == 0 || strcmp(tok_a, "https") == 0));

            // Disambiguate tok_a.
            if (tok_a) {
                if (looks_like_host) {
                    // tok_a = host, then proto, then label
                    host = tok_a;
                    int b_proto = (tok_b && (strcmp(tok_b, "http") == 0 || strcmp(tok_b, "https") == 0));
                    if (b_proto) {
                        proto = tok_b;
                        if (tok_c) label = tok_c;
                    } else if (tok_b) {
                        label = tok_b;
                    }
                } else if (is_proto) {
                    // tok_a = proto, then host, then label
                    proto = tok_a;
                    int b_host = (tok_b && (*tok_b != 0) && strchr(tok_b, '.') != NULL);
                    if (b_host) {
                        host = tok_b;
                        if (tok_c) label = tok_c;
                    } else if (tok_b) {
                        label = tok_b;
                    }
                } else {
                    // tok_a = label (no dots, not proto)
                    label = tok_a;
                    int b_host  = (tok_b && (*tok_b != 0) && strchr(tok_b, '.') != NULL);
                    int b_proto = (tok_b && (*tok_b != 0)
                                   && (strcmp(tok_b, "http") == 0 || strcmp(tok_b, "https") == 0));
                    if (b_host) {
                        host = tok_b;
                        int c_proto = (tok_c && (*tok_c != 0)
                                       && (strcmp(tok_c, "http") == 0 || strcmp(tok_c, "https") == 0));
                        if (c_proto) proto = tok_c;
                    } else if (b_proto) {
                        proto = tok_b;
                        int c_host = (tok_c && (*tok_c != 0) && strchr(tok_c, '.') != NULL);
                        if (c_host) host = tok_c;
                    }
                }
            }

            long lp = strtol(tok_local, NULL, 10);
            long up = strtol(tok_up, NULL, 10);

            if (lp < 1024 || lp > 65535) {
                char eb[96];
                snprintf(eb, sizeof(eb), "ERR: local_port must be 1024..65535 (got %ld)", lp);
                telegram_send_text(chat_id, eb);
                return;
            }
            if (up < 1 || up > 65535) {
                char eb[96];
                snprintf(eb, sizeof(eb), "ERR: upstream_port must be 1..65535 (got %ld)", up);
                telegram_send_text(chat_id, eb);
                return;
            }

            esp_err_t err = portmap_add((uint16_t)lp,
                                        host,
                                        (uint16_t)up,
                                        proto,
                                        label);
            if (err == ESP_OK) {
                char ack[256];
                snprintf(ack, sizeof(ack),
                         "OK: added :%ld -> %s://%s:%ld [%s]",
                         lp,
                         proto,
                         host[0] ? host : "(auto)",
                         up,
                         label[0] ? label : "");
                telegram_send_text(chat_id, ack);
            } else if (err == ESP_ERR_INVALID_STATE) {
                char eb[80];
                snprintf(eb, sizeof(eb), "ERR: port :%ld already mapped", lp);
                telegram_send_text(chat_id, eb);
            } else if (err == ESP_ERR_NO_MEM) {
                telegram_send_text(chat_id, "ERR: max mappings reached (8). Delete one first.");
            } else if (err == ESP_ERR_INVALID_ARG) {
                telegram_send_text(chat_id, "ERR: invalid argument");
            } else {
                char eb[80];
                snprintf(eb, sizeof(eb), "ERR: add failed (%s)", esp_err_to_name(err));
                telegram_send_text(chat_id, eb);
            }
        } else {
            telegram_send_text(chat_id,
                "ERR: unknown portmap subcommand. Use /portmap.");
        }
    } else if ((cmd_len == 4 && strncmp(cmd, "logs", 4) == 0) ||
               (cmd_len == 3 && strncmp(cmd, "log", 3) == 0)) {
        size_t buf_size = 65536;
        char *log_buf = NULL;
        if (esp_psram_is_initialized()) {
            log_buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
        }
        if (log_buf == NULL) {
            buf_size = 16384;
            log_buf = malloc(buf_size);
        }
        if (log_buf == NULL) {
            telegram_send_text(chat_id, "ERR: out of memory allocating log buffer");
            return;
        }
        size_t len = system_health_get_live_logs(log_buf, buf_size);
        if (len == 0) {
            telegram_send_text(chat_id, "No hay logs disponibles en el búfer.");
            free(log_buf);
            return;
        }
        char cap[96];
        snprintf(cap, sizeof(cap), "Logs en vivo del ESP32 (%zu bytes)", len);
        esp_err_t doc_err = telegram_send_document(chat_id, "esp32_log.txt", log_buf, len, cap);
        if (doc_err != ESP_OK) {
            telegram_send_text(chat_id, "ERR: no se pudo enviar el archivo de logs");
        }
        free(log_buf);
    } else if (cmd_len == 8 && strncmp(cmd, "crashlog", 8) == 0) {
        char crash_buf[4096];
        size_t len = system_health_get_crash_logs(crash_buf, sizeof(crash_buf));
        if (len == 0) {
            telegram_send_text(chat_id, "No hay registro de cuelgues previos en memoria RTC.");
            return;
        }
        esp_err_t doc_err = telegram_send_document(chat_id, "crash_log.txt", crash_buf, len,
                                                   "Log previo al último reinicio/cuelgue (memoria RTC)");
        if (doc_err != ESP_OK) {
            telegram_send_text(chat_id, "ERR: no se pudo enviar crash_log.txt");
        }
    } else if (cmd_len == 4 && strncmp(cmd, "help", 4) == 0) {
        char help[1024];
        snprintf(help, sizeof(help),
                 "Commands:\n"
                 "/wake - WOL magic packet\n"
                 "/status - fast status (no TCP probe, uses cached state)\n"
                 "/stats - server resource stats (CPU, RAM, disk, Docker)\n"
                 "/ping - live TCP probe to cocopa01\n"
                 "/diag - heap/sockets/tasks diagnostics\n"
                 "/logs - download live system logs as .txt file\n"
                 "/crashlog - download last crash/stall log (RTC)\n"
                 "/info - hardware info\n"
                 "/off - power off cocopa01 (auto-wake if S3)\n"
                 "/suspend - S3 RAM sleep\n"
                 "/schedule - scheduled daily wake/off (set wake|off HH:MM, reset)\n"
                 "/reboot - reboot ESP32 now, or set timer (/reboot help)\n"
                 "/automation - show inactivity automation config+status\n"
                 "/autoset <key> <val> - configure automation\n"
                 "/portmap list|add|del|defaults - manage port redirections\n"
                 "/listauth - authorized chat IDs\n"
                 "/help - this message");
        telegram_send_text(chat_id, help);
    } else {
        telegram_send_text(chat_id, "Unknown command. Use /help.");
    }
}

// Cliente HTTP de envio (sendMessage). Se crea y destruye DENTRO de
// telegram_send_text() — explícitamente NO global. Antes era un puntero
// global (s_send_client) compartido entre 5 tasks (poll de Telegram,
// watchdog, scheduler, automation y el httpd de /test_tg) sin mutex: el
// comentario "solo hay UN consumidor" era falso y dos tasks en carrera
// podían intercalar init/perform/cleanup sobre el mismo handle (use-after-
// free / corrupción de heap). Cada llamada crea su propio cliente con
// keep-alive off y lo destruye SIEMPRE al final: garantiza que un socket
// TLS medio-muerto no bloquee el sistema ni se acumule.
esp_err_t telegram_send_text(int64_t chat_id, const char *text) {
    if (text == NULL || *text == 0) return ESP_OK;
    char url[256];
    tg_url(url, sizeof(url), "sendMessage");

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "chat_id", (double)chat_id);
    cJSON_AddStringToObject(root, "text", text);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) return ESP_ERR_NO_MEM;

    // Cliente LOCAL: nada compartido entre tasks (ver comentario arriba).
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = TG_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "sendMessage init failed (heap/sockets?)");
        free(body);
        return ESP_FAIL;
    }
    esp_http_client_set_url(client, url);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : -1;
    esp_err_t result = ESP_OK;

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sendMessage failed: %s", esp_err_to_name(err));
        result = err;
    } else if (status != 200) {
        ESP_LOGW(TAG, "sendMessage HTTP %d", status);
        result = ESP_FAIL;
    } else {
        ESP_LOGI(TAG, "sent to %lld (%zu chars)", (long long)chat_id, strlen(text));
    }

    // SIEMPRE destruir el cliente, haya ido bien o mal. Asi no acumulamos
    // sockets TLS potencialmente half-open que encolen bloqueos futuros.
    esp_http_client_cleanup(client);
    free(body);
    return result;
}

esp_err_t telegram_broadcast(const char *text) {
    if (text == NULL || *text == 0) return ESP_OK;
    if (g_auth_ids_count == 0) return ESP_OK;
    for (int i = 0; i < g_auth_ids_count; i++) {
        esp_err_t err = telegram_send_text(g_auth_ids[i], text);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "broadcast to %lld failed: %s",
                     (long long)g_auth_ids[i], esp_err_to_name(err));
        }
    }
    return ESP_OK;
}

esp_err_t telegram_send_document(int64_t chat_id, const char *filename,
                                const char *file_content, size_t file_size,
                                const char *caption) {
    if (filename == NULL || file_content == NULL || file_size == 0) return ESP_ERR_INVALID_ARG;
    char url[256];
    tg_url(url, sizeof(url), "sendDocument");

    const char *boundary = "----ESP32TelegramBoundary7MA4YWxkTrZu0gW";
    char header_buf[512];
    int hlen = 0;
    if (caption != NULL && *caption != 0) {
        hlen = snprintf(header_buf, sizeof(header_buf),
            "--%s\r\n"
            "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n"
            "%lld\r\n"
            "--%s\r\n"
            "Content-Disposition: form-data; name=\"caption\"\r\n\r\n"
            "%s\r\n"
            "--%s\r\n"
            "Content-Disposition: form-data; name=\"document\"; filename=\"%s\"\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n\r\n",
            boundary,
            (long long)chat_id,
            boundary,
            caption,
            boundary,
            filename);
    } else {
        hlen = snprintf(header_buf, sizeof(header_buf),
            "--%s\r\n"
            "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n"
            "%lld\r\n"
            "--%s\r\n"
            "Content-Disposition: form-data; name=\"document\"; filename=\"%s\"\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n\r\n",
            boundary,
            (long long)chat_id,
            boundary,
            filename);
    }

    const char *tail = "\r\n--";
    const char *tail2 = "----ESP32TelegramBoundary7MA4YWxkTrZu0gW--\r\n";
    size_t tail_len = strlen(tail) + strlen(tail2);

    size_t total_body_len = hlen + file_size + tail_len;

    char *body = NULL;
    if (esp_psram_is_initialized()) {
        body = heap_caps_malloc(total_body_len + 1, MALLOC_CAP_SPIRAM);
    }
    if (body == NULL) {
        body = malloc(total_body_len + 1);
    }
    if (body == NULL) {
        ESP_LOGE(TAG, "sendDocument: OOM allocating %zu bytes for multipart body", total_body_len);
        return ESP_ERR_NO_MEM;
    }

    memcpy(body, header_buf, hlen);
    memcpy(body + hlen, file_content, file_size);
    char *p = body + hlen + file_size;
    memcpy(p, "\r\n--", 4);
    p += 4;
    memcpy(p, boundary, strlen(boundary));
    p += strlen(boundary);
    memcpy(p, "--\r\n", 4);
    p += 4;
    size_t final_len = p - body;

    char content_type[128];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "sendDocument init failed");
        free(body);
        return ESP_FAIL;
    }
    esp_http_client_set_url(client, url);
    esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_post_field(client, body, final_len);

    esp_err_t err = esp_http_client_perform(client);
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : -1;
    esp_err_t result = ESP_OK;

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sendDocument failed: %s", esp_err_to_name(err));
        result = err;
    } else if (status != 200) {
        ESP_LOGW(TAG, "sendDocument HTTP %d", status);
        result = ESP_FAIL;
    } else {
        ESP_LOGI(TAG, "sendDocument '%s' sent to %lld (%zu bytes)",
                 filename, (long long)chat_id, file_size);
    }

    esp_http_client_cleanup(client);
    free(body);
    return result;
}

esp_err_t telegram_broadcast_document(const char *filename,
                                      const char *file_content, size_t file_size,
                                      const char *caption) {
    if (filename == NULL || file_content == NULL || file_size == 0) return ESP_OK;
    if (g_auth_ids_count == 0) return ESP_OK;
    for (int i = 0; i < g_auth_ids_count; i++) {
        esp_err_t err = telegram_send_document(g_auth_ids[i], filename, file_content, file_size, caption);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "broadcast_document to %lld failed: %s",
                     (long long)g_auth_ids[i], esp_err_to_name(err));
        }
    }
    return ESP_OK;
}

// Persiste g_last_update_id en NVS para que sobreviva al reinicio.
// Al arrancar, telegram_init() lo restaura y el primer getUpdates pide
// offset=last_uid+1, descartando automáticamente los comandos ya procesados.
esp_err_t telegram_save_last_uid(void) {
    if (g_last_update_id <= 0) return ESP_OK;
    nvs_handle_t h;
    if (nvs_open("telegram", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i64(h, "last_uid", g_last_update_id);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "saved last_uid=%lld to NVS", (long long)g_last_update_id);
    }
    return ESP_OK;
}

static void process_update(cJSON *update) {
    const cJSON *msg = cJSON_GetObjectItemCaseSensitive(update, "message");
    if (msg == NULL || !cJSON_IsObject(msg)) return;

    const cJSON *chat = cJSON_GetObjectItemCaseSensitive(msg, "chat");
    const cJSON *chat_id_node = (chat != NULL) ? cJSON_GetObjectItemCaseSensitive(chat, "id") : NULL;
    const cJSON *text_node = cJSON_GetObjectItemCaseSensitive(msg, "text");
    const cJSON *update_id_node = cJSON_GetObjectItemCaseSensitive(update, "update_id");

    if (!cJSON_IsNumber(chat_id_node)) return;
    int64_t chat_id = (int64_t)chat_id_node->valuedouble;
    if (update_id_node != NULL && cJSON_IsNumber(update_id_node)) {
        int64_t uid = (int64_t)update_id_node->valuedouble;
        if (uid > g_last_update_id) g_last_update_id = uid;
    }
    if (!is_authorized(chat_id)) {
        ESP_LOGI(TAG, "Unauthorized from %lld -- ignored", (long long)chat_id);
        return;
    }
    if (!cJSON_IsString(text_node) || text_node->valuestring == NULL) return;

    const char *text = text_node->valuestring;
    size_t cmd_len = 0;
    const char *cmd = cmd_name(text, &cmd_len);
    if (cmd == NULL) {
        telegram_send_text(chat_id, "Unknown command. Use /help.");
        return;
    }
    ESP_LOGI(TAG, "command from %lld: /%.*s", (long long)chat_id, (int)cmd_len, cmd);
    // Cualquier comando autorizado resetea los temporizadores de inactividad.
    automation_kick();
    // `args` apunta justo después del comando (saltando mención @bot y espacios).
    const char *args = cmd + cmd_len;
    if (*args == '@') {
        while (*args && *args != ' ' && *args != '\t' && *args != '\n' && *args != '\r') args++;
    }
    while (*args == ' ' || *args == '\t') args++;
    handle_command(chat_id, cmd, cmd_len, args);
}

// Cliente HTTP persistente para el poll: reutiliza la conexion TLS entre
// ciclos (keep-alive). Recrear la conexion en cada poll dejaba ~1 socket
// nuevo en TIME_WAIT cada ~6s (TIME_WAIT = 2*MSL = 120s con
// CONFIG_LWIP_TCP_MSL=60000), saturando los 16 sockets de lwIP: el poll
// fallaba en silencio hasta que expiraba el TIME_WAIT mas viejo -> demoras
// de hasta 2 minutos en responder.
#define TG_POLL_BUF_SIZE 8192
static esp_http_client_handle_t s_poll_client = NULL;
static char s_poll_buf[TG_POLL_BUF_SIZE];
static int s_poll_len = 0;
// Contador de polls exitosos consecutivos: al llegar a TG_POLL_RECREATE_AFTER
// tiramos el cliente y dejamos que el proximo ciclo lo recree con TLS handshake
// limpio. Defiende contra el socket TLS half-open tras horas de uptime
// (api.telegram.org / edge intermedio cierra la conexion idle sin FIN
// detectable, y esp_http_client_perform con keep-alive se cuelga >30s sin
// retornar error).
static uint32_t s_poll_success_count = 0;

static esp_err_t poll_event_handler(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data != NULL && evt->data_len > 0) {
        int room = TG_POLL_BUF_SIZE - s_poll_len - 1;
        int n = (evt->data_len < room) ? evt->data_len : room;
        if (n > 0) {
            memcpy(s_poll_buf + s_poll_len, evt->data, n);
            s_poll_len += n;
            s_poll_buf[s_poll_len] = 0;
        }
    }
    return ESP_OK;
}

static void poll_client_teardown(void) {
    if (s_poll_client != NULL) {
        esp_http_client_cleanup(s_poll_client);
        s_poll_client = NULL;
    }
}

static void long_poll_once(void) {
    char full_url[512];
    snprintf(full_url, sizeof(full_url),
             "https://%s/bot%s/getUpdates?offset=%lld&timeout=%d",
             TG_API_HOST, CONFIG_TELEGRAM_BOT_TOKEN,
             (long long)(g_last_update_id + 1), TG_POLL_TIMEOUT_S);

    if (s_poll_client == NULL) {
        esp_http_client_config_t config = {
            .url = "https://" TG_API_HOST,
            .method = HTTP_METHOD_GET,
            .timeout_ms = (TG_POLL_TIMEOUT_S + 5) * 1000,
            .buffer_size = 4096,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .event_handler = poll_event_handler,
            .keep_alive_enable = true,
        };
        s_poll_client = esp_http_client_init(&config);
        if (s_poll_client == NULL) {
            // Antes esto retornaba sin loguear nada: fallos por agotamiento
            // de sockets eran invisibles en el log serial.
            ESP_LOGE(TAG, "poll client init failed (heap/sockets?)");
            return;
        }
    }

    s_poll_len = 0;
    s_poll_buf[0] = 0;

    esp_http_client_set_url(s_poll_client, full_url);
    esp_err_t err = esp_http_client_perform(s_poll_client);
    if (err != ESP_OK) {
        // Conexion rota (idle timeout del server/NAT tras el long-poll):
        // tirar el cliente y reconectar en el proximo ciclo.
        ESP_LOGW(TAG, "poll err=%s -- recreating client", esp_err_to_name(err));
        poll_client_teardown();
        s_poll_success_count = 0;
        return;
    }

    int status = esp_http_client_get_status_code(s_poll_client);
    int total = s_poll_len;

    if (status != 200) {
        ESP_LOGW(TAG, "poll status=%d body=%.120s", status, s_poll_buf);
        if (status == 409) {
            ESP_LOGE(TAG, "getUpdates 409: otra instancia esta polleando "
                          "getUpdates con este mismo token");
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
        // Cualquier status no-200 cierra el socket para reconectar limpiamente
        poll_client_teardown();
        s_poll_success_count = 0;
        return;
    }

    // Poll HTTP 200 OK: bumpeamos el contador de "poll bueno" y, al
    // alcanzar TG_POLL_RECREATE_AFTER, recreamos el cliente de cero. Asi
    // evitamos que un socket TLS half-open (que cerraria perform() en un
    // futuro ciclo con un cuelgue de >30s sin error) quede vivo por horas.
    s_poll_success_count++;
    if (s_poll_success_count >= TG_POLL_RECREATE_AFTER) {
        ESP_LOGI(TAG, "poll client recycled after %u successful polls (preventive)",
                 (unsigned)s_poll_success_count);
        poll_client_teardown();
        s_poll_success_count = 0;
    }

    ESP_LOGI(TAG, "poll offset=%lld status=%d body_len=%d",
             (long long)(g_last_update_id + 1), status, total);
    if (total > 50) {
        ESP_LOGI(TAG, "poll body: %.200s", s_poll_buf);
    }

    if (total == 0) return;

    cJSON *root = cJSON_Parse(s_poll_buf);
    if (root == NULL) {
        ESP_LOGW(TAG, "poll JSON parse error");
        return;
    }
    cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (cJSON_IsArray(result)) {
        int cnt = cJSON_GetArraySize(result);
        if (cnt > 0) {
            ESP_LOGI(TAG, "poll got %d updates -- releasing poll TLS for command replies", cnt);
            // Liberamos el cliente de polling para que telegram_send_text() disponga
            // de toda la memoria interna SRAM y sockets libres para responder comandos.
            poll_client_teardown();
            s_poll_success_count = 0;
            int64_t prev_uid = g_last_update_id;
            cJSON *upd = NULL;
            cJSON_ArrayForEach(upd, result) {
                process_update(upd);
            }
            if (g_last_update_id > prev_uid) {
                telegram_save_last_uid();
            }
        }
    }
    cJSON_Delete(root);
}

void telegram_poll_task(void *arg) {
    ESP_LOGI(TAG, "poll task started");

    // Aviso único por boot si main.c detectó que la NVS fue borrada por
    // el erase-on-error. Se consume apenas arrancamos (Telegram ya está
    // inicializado por telegram_init() antes de crear este task) así que
    // broadcast() puede salir en este mismo ciclo. Si NO hubo wipe, el
    // consume devuelve false y seguimos al loop normal sin ruido.
    if (scheduler_consume_nvs_wiped()) {
        ESP_LOGW(TAG, "NVS fue borrada en este boot — avisando por Telegram");
        telegram_broadcast(
            "⚠️ ESP32 reinició con NVS borrada (erase-on-error). "
            "Perdiste: /schedule, /autoset, /portmap defaults, cocopa01 boot_time, "
            "cert funnel, peers cacheados de Tailscale. "
            "Reconfigurar: /schedule set wake HH:MM, /autoset, /portmap defaults. "
            "Serial: `acme renew` si querés re-emitir el cert ya.");
    }

    // Notificar si el reinicio anterior fue por recuperación de cuelgue
    health_crash_report_t cr;
    if (system_health_check_crash_report(&cr, true)) {
        ESP_LOGW(TAG, "Notifying Telegram about watchdog recovery: task '%s', reason: %s",
                 cr.failing_task, cr.reason);
        char alert[320];
        snprintf(alert, sizeof(alert),
                 "⚠️ *Recuperación Automática de Watchdog*\n"
                 "El ESP32 se recuperó de un bloqueo en la tarea '%s' (%s).\n"
                 "Uptime previo: %lu min (%lu h %lu m).\n"
                 "Total recuperaciones: %u.\n"
                 "Adjuntando crash_log.txt previo al reinicio...",
                 cr.failing_task[0] ? cr.failing_task : "desconocida",
                 cr.reason,
                 (unsigned long)(cr.prev_uptime_s / 60),
                 (unsigned long)(cr.prev_uptime_s / 3600),
                 (unsigned long)((cr.prev_uptime_s % 3600) / 60),
                 (unsigned)cr.reset_count);
        telegram_broadcast(alert);

        // Enviar automáticamente el archivo crash_log.txt con el log pre-reinicio
        char crash_buf[4096];
        size_t crash_len = system_health_get_crash_logs(crash_buf, sizeof(crash_buf));
        if (crash_len > 0) {
            telegram_broadcast_document("crash_log.txt", crash_buf, crash_len,
                                        "Log previo al cuelgue (memoria RTC)");
            system_health_clear_crash_logs();
        }
    }

    while (1) {
        system_health_heartbeat(HEALTH_CHANNEL_TELEGRAM);
        if (!wifi_is_connected()) {
            poll_client_teardown();
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        long_poll_once();
        system_health_heartbeat(HEALTH_CHANNEL_TELEGRAM);
        vTaskDelay(pdMS_TO_TICKS(TG_POLL_INTERVAL_MS));
    }
}
void telegram_init(void) {
    g_auth_ids_count = parse_auth_ids(CONFIG_TELEGRAM_AUTHORIZED_IDS);
    ESP_LOGI(TAG, "telegram_init: %d authorized chat IDs", g_auth_ids_count);

    nvs_handle_t h;
    if (nvs_open("telegram", NVS_READONLY, &h) == ESP_OK) {
        int64_t saved_uid = 0;
        if (nvs_get_i64(h, "last_uid", &saved_uid) == ESP_OK && saved_uid > 0) {
            g_last_update_id = saved_uid;
            ESP_LOGI(TAG, "restored last_update_id from NVS: %lld", (long long)g_last_update_id);
        }
        nvs_close(h);
    }
}