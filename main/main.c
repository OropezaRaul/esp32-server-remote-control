#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <inttypes.h>
#include <esp_log.h>
#include <esp_chip_info.h>
#include <esp_clk_tree.h>
#include <esp_flash.h>
#include <esp_psram.h>
#include <esp_system.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <soc/clk_tree_defs.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "wifi.h"
#include "wol.h"
#include "server_client.h"
#include "ready_probe.h"
#include "state.h"
#include "telegram.h"
#include "wake.h"
#include "web_server.h"
#include "microlink_wrapper.h"
#include "microlink_watchdog.h"
#include "automation.h"
#include "scheduler.h"
#include "auto_reboot.h"
#include "portmap.h"
#include "system_health.h"
#include "version.h"

#if CONFIG_ML_ACME_ENABLED
#include "ml_acme.h"
void acme_request_renew(void);
#endif

#include <stdlib.h>

#include "sdkconfig.h"

static const char *TAG = "main";

static void print_system_info(void) {
    ESP_LOGI(TAG, "=== %s ===", FIRMWARE_VERSION);

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t cpu_freq_hz = 0;
    esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_CPU, ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &cpu_freq_hz);
    ESP_LOGI(TAG, "Chip: ESP32-S3 rev v%d.%d, %d cores @ %" PRIu32 " MHz",
             chip.revision / 16, chip.revision % 16,
             chip.cores, cpu_freq_hz / 1000000U);

    if (esp_psram_is_initialized()) {
        ESP_LOGI(TAG, "PSRAM: %d MB total, %d KB free",
                 esp_psram_get_size() / (1024 * 1024),
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    } else {
        ESP_LOGI(TAG, "PSRAM: NOT FOUND");
    }

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    ESP_LOGI(TAG, "Flash: %" PRIu32 " MB", flash_size / (1024 * 1024));
    ESP_LOGI(TAG, "Heap free: %u bytes", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    ESP_LOGI(TAG, "Wi-Fi MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void handle_serial_command(char *line) {
    while (*line && isspace((unsigned char)*line)) line++;
    size_t len = strlen(line);
    while (len > 0 && isspace((unsigned char)line[len - 1])) { line[--len] = 0; }
    for (char *p = line; *p; ++p) *p = (char)tolower((unsigned char)*p);

    if (strcmp(line, "info") == 0) {
        print_system_info();
        esp_ip4_addr_t ip = wifi_get_ip();
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&ip));
    } else if (strcmp(line, "wake") == 0) {
        if (wake_request("serial") == ESP_OK) {
            state_mark_known_on();
            ESP_LOGI(TAG, "wake solicitado desde serial: %s", wake_status_text());
        }
    } else if (strcmp(line, "off") == 0) {
        ESP_LOGI(TAG, "server TCP/22 alive: %s",
                 server_tcp_alive(2000) ? "YES" : "NO");
        if (server_send_off() == ESP_OK) {
            state_set(SERVER_STATE_OFF);
        }
    } else if (strcmp(line, "suspend") == 0) {
        ESP_LOGI(TAG, "server TCP/22 alive: %s",
                 server_tcp_alive(2000) ? "YES" : "NO");
        if (server_send_suspend() == ESP_OK) {
            state_set(SERVER_STATE_SUSPEND);
        }
    } else if (strcmp(line, "state") == 0) {
        ESP_LOGI(TAG, "state: %s",
                 server_state_str(state_get()));
    } else if (strncmp(line, "tailscale", 9) == 0) {
        char *arg = line + 9;
        while (*arg && isspace((unsigned char)*arg)) arg++;
        if (*arg == 0 || strcmp(arg, "status") == 0) {
            ESP_LOGI(TAG, "tailscale state=%s vpn_ip=%lu connected=%d",
                     microlink_wrapper_state_name(microlink_wrapper_get_state()),
                     (unsigned long)microlink_wrapper_get_vpn_ip(),
                     microlink_wrapper_is_connected() ? 1 : 0);
            char buf[400];
            microlink_watchdog_format_status(buf, sizeof(buf));
            ESP_LOGI(TAG, "%s", buf);
        } else if (strcmp(arg, "reset") == 0) {
            ESP_LOGW(TAG, "forcing ESP32 reboot via serial command");
            esp_err_t err = microlink_watchdog_force_recover();
            ESP_LOGI(TAG, "tailscale reset result: %s", esp_err_to_name(err));
        } else if (strcmp(arg, "rebind") == 0) {
            ESP_LOGW(TAG, "forcing Tailscale rebind via serial command (diagnostic only)");
            esp_err_t err = microlink_wrapper_rebind();
            ESP_LOGI(TAG, "tailscale rebind result: %s", esp_err_to_name(err));
        } else {
            ESP_LOGW(TAG, "unknown tailscale subcommand: '%s' (try: status, reset, rebind)", arg);
        }
#if CONFIG_ML_ACME_ENABLED
    } else if (strcmp(line, "acme renew") == 0) {
        acme_request_renew();
        ESP_LOGI(TAG, "acme: renewal requested (task picks it up in <=30s)");
#endif
    } else if (strncmp(line, "reboot", 6) == 0) {
        char *arg = line + 6;
        while (*arg && isspace((unsigned char)*arg)) arg++;
        if (*arg == 0 || strcmp(arg, "now") == 0) {
            ESP_LOGW(TAG, "forcing ESP32 reboot via serial command");
            auto_reboot_trigger("serial reboot", 0);
        } else if (strcmp(arg, "status") == 0) {
            char buf[AUTO_REBOOT_STATUS_BUF_SIZE];
            auto_reboot_format_status(buf, sizeof(buf));
            ESP_LOGI(TAG, "%s", buf);
        } else if (strcmp(arg, "off") == 0 || strcmp(arg, "disable") == 0) {
            auto_reboot_set_interval_min(0);
            ESP_LOGI(TAG, "auto-reboot disabled (switch off)");
        } else {
            char *endptr = NULL;
            long val = strtol(arg, &endptr, 10);
            if (endptr != arg) {
                while (*endptr && isspace((unsigned char)*endptr)) endptr++;
                if (*endptr == 'h' || *endptr == 'H') {
                    val *= 60;
                    endptr++;
                } else if (*endptr == 'm' || *endptr == 'M') {
                    endptr++;
                }
                while (*endptr && isspace((unsigned char)*endptr)) endptr++;
                if (*endptr == 0 && val >= 0 && val <= 10080) {
                    auto_reboot_set_interval_min((uint32_t)val);
                    if (val == 0) {
                        ESP_LOGI(TAG, "auto-reboot disabled (switch off)");
                    } else {
                        ESP_LOGI(TAG, "auto-reboot interval set to %ld min", val);
                    }
                } else {
                    ESP_LOGW(TAG, "usage: reboot [now|<min>|status|off]");
                }
            } else {
                ESP_LOGW(TAG, "usage: reboot [now|<min>|status|off]");
            }
        }
    } else if (strcmp(line, "help") == 0) {
        ESP_LOGI(TAG, "Commands:");
        ESP_LOGI(TAG, "  info              - system info");
        ESP_LOGI(TAG, "  wake              - send WOL magic packet");
        ESP_LOGI(TAG, "  off               - send /off to server");
        ESP_LOGI(TAG, "  suspend           - send /suspend to server");
        ESP_LOGI(TAG, "  state             - print current server state");
        ESP_LOGI(TAG, "  tailscale status  - Tailscale state + watchdog status");
        ESP_LOGI(TAG, "  tailscale rebind  - force Tailscale rebind (lightweight, diagnostic only)");
        ESP_LOGI(TAG, "  tailscale reset   - force ESP32 reboot (the watchdog's recovery action)");
        ESP_LOGI(TAG, "  reboot            - reboot ESP32 now or set timer (reboot [now|<min>|status|off])");
#if CONFIG_ML_ACME_ENABLED
        ESP_LOGI(TAG, "  acme renew        - force ACME cert re-issuance + TLS reload");
#endif
        ESP_LOGI(TAG, "  help              - this help");
    } else {
        ESP_LOGW(TAG, "Unknown command: '%s' (type 'help')", line);
    }
}

static void serial_task(void *arg) {
    char buf[128];
    size_t idx = 0;
    while (1) {
        int c = getchar();
        if (c == EOF || c < 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (c == '\r') continue;
        if (c == '\n') {
            buf[idx] = 0;
            if (idx > 0) {
                handle_serial_command(buf);
            }
            idx = 0;
            continue;
        }
        if (idx + 1 < sizeof(buf)) {
            buf[idx++] = (char)c;
        } else {
            idx = 0;
        }
    }
}

#if CONFIG_ML_ACME_ENABLED




static esp_err_t acme_publish_dns(const char *name, const char *value,
                                  uint32_t timeout_ms) {
    return microlink_wrapper_set_dns(name, value, timeout_ms);
}

static volatile bool s_acme_force = false;
void acme_request_renew(void) { s_acme_force = true; }

static void acme_task(void *arg) {
    
    for (;;) {
        if (microlink_wrapper_is_connected() &&
            microlink_wrapper_get_fqdn() != NULL) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    const char *fqdn = microlink_wrapper_get_fqdn();
    ESP_LOGI(TAG, "acme: starting issuer for %s", fqdn);

    ml_acme_set_dns_publisher(acme_publish_dns);
    ml_acme_set_directory(CONFIG_ML_ACME_DIRECTORY_URL);
    if (ml_acme_init() != ESP_OK) {
        ESP_LOGE(TAG, "acme: key init failed, task exiting");
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        int64_t last = ml_acme_last_issue_unix();
        int64_t now = (int64_t)time(NULL);
        bool need = s_acme_force || (last == 0) || (now - last > 60 * 24 * 3600);
        s_acme_force = false;
        if (need) {
            ESP_LOGI(TAG, "acme: issuing cert for %s", fqdn);
            esp_err_t err = ml_acme_obtain_cert(fqdn);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "acme: cert issued + stored, reloading funnel TLS");
                web_server_funnel_tls_reload();
            } else {
                ESP_LOGE(TAG, "acme: issuance failed: %s", ml_acme_last_error());
            }
        }
        
        for (int i = 0; i < 2880 && !s_acme_force; i++) {
            vTaskDelay(pdMS_TO_TICKS(30000));
        }
    }
}
#endif

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // PELIGRO: este erase borra TODA la NVS — namespace `schedule`
        // (wake/off programados), `automation` (idle timers), `portmap`
        // (mappings TCP), `server/boot_time`, y los blobs de microlink
        // (peers cacheados, cert funnel, estado ACME). Después de este
        // log el usuario va a tener que re-configurar `/schedule`,
        // `/autoset`, `/portmap` desde Telegram. Marcamos el flag
        // AHORA (antes del erase) para que telegram_poll_task lo
        // consuma apenas esté vivo y le avise al usuario por Telegram.
        ESP_LOGW(TAG, "NVS erase-on-error (%s) — TODAS las configs (schedule/automation/portmap/etc.) se perdieron",
                 esp_err_to_name(ret));
        scheduler_mark_nvs_wiped();
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    print_system_info();

    esp_err_t health_err = system_health_init();
    if (health_err != ESP_OK) {
        ESP_LOGE(TAG, "system_health_init failed: %s", esp_err_to_name(health_err));
    }

    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", CONFIG_WIFI_SSID);
    esp_err_t err = wifi_init_sta();
    if (err != ESP_OK) {
        





        ESP_LOGW(TAG, "WiFi sin conexión en el primer intento (%s). "
                      "Arrancando subsistemas igual; wifi_keepalive reintenta en background.",
                 esp_err_to_name(err));
    } else {
        esp_ip4_addr_t ip = wifi_get_ip();
        ESP_LOGI(TAG, "WiFi connected. IP: " IPSTR, IP2STR(&ip));
    }

    {
        // Supervisor de Auto-WOL: debe existir ANTES de web_server_start para
        // que ningun handler (/app, /wol...) lo encuentre a medio iniciar.
        esp_err_t wk_err = wake_init();
        if (wk_err != ESP_OK) {
            ESP_LOGE(TAG, "wake_init failed: %s", esp_err_to_name(wk_err));
        }

        // Sonda de readiness: tambien antes de web_server_start, porque los
        // handlers (/app, /ready) leen su latch. Es la pieza que reemplaza
        // la espera por reloj de la 1.8.2.
        esp_err_t rd_err = ready_init();
        if (rd_err != ESP_OK) {
            ESP_LOGE(TAG, "ready_init failed: %s (el portal usara el techo de seguridad)",
                     esp_err_to_name(rd_err));
        }

        esp_err_t ws_err = web_server_start();
        if (ws_err != ESP_OK) {
            ESP_LOGE(TAG, "web_server_start failed: %s", esp_err_to_name(ws_err));
        }

        esp_err_t ws_funnel_err;
#if CONFIG_ESP_HTTPS_SERVER_ENABLE
        

        ws_funnel_err = web_server_funnel_tls_start();
        if (ws_funnel_err != ESP_OK) {
            ESP_LOGE(TAG, "web_server_funnel_tls_start failed: %s", esp_err_to_name(ws_funnel_err));
        }
#else
        ws_funnel_err = web_server_funnel_start();
        if (ws_funnel_err != ESP_OK) {
            ESP_LOGE(TAG, "web_server_funnel_start failed: %s", esp_err_to_name(ws_funnel_err));
        }
#endif

        esp_err_t ml_err = microlink_wrapper_start();
        if (ml_err != ESP_OK) {
            ESP_LOGE(TAG, "microlink_wrapper_start failed: %s", esp_err_to_name(ml_err));
        }

        state_init();
        telegram_init();
        xTaskCreate(state_poll_task, "state_poll", 4096, NULL, 4, NULL);
        xTaskCreate(telegram_poll_task, "telegram", 16384, NULL, 5, NULL);

        esp_err_t wd_err = microlink_watchdog_init();
        if (wd_err != ESP_OK) {
            ESP_LOGE(TAG, "microlink_watchdog_init failed: %s", esp_err_to_name(wd_err));
        }

        esp_err_t auto_err = automation_init();
        if (auto_err != ESP_OK) {
            ESP_LOGE(TAG, "automation_init failed: %s", esp_err_to_name(auto_err));
        }

        // Scheduler de wake/off diario (schedule.c). Va después de
        // automation_init() porque depende de SNTP (automation lo inicia);
        // no dispara nada hasta que la hora está sincronizada.
        esp_err_t sched_err = scheduler_init();
        if (sched_err != ESP_OK) {
            ESP_LOGE(TAG, "scheduler_init failed: %s", esp_err_to_name(sched_err));
        }

        esp_err_t ar_err = auto_reboot_init();
        if (ar_err != ESP_OK) {
            ESP_LOGE(TAG, "auto_reboot_init failed: %s", esp_err_to_name(ar_err));
        }

#if CONFIG_ML_ACME_ENABLED
        

        xTaskCreate(acme_task, "acme", 12288, NULL, 3, NULL);
#endif

        esp_err_t pm_err = portmap_init();
        if (pm_err != ESP_OK) {
            ESP_LOGE(TAG, "portmap_init failed: %s", esp_err_to_name(pm_err));
        }
    }

    ESP_LOGI(TAG, "Boot complete. Type 'help' for serial commands.");
    xTaskCreate(serial_task, "serial", 4096, NULL, 5, NULL);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}