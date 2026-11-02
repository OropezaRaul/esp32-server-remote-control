#include "funnel_ctl.h"

#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <stdio.h>

#include "microlink_wrapper.h"
#include "ml_cert.h"
#include "telegram.h"
#include "wifi.h"
#include "sdkconfig.h"
#include "sockmgr.h"

static const char *TAG = "funnel_ctl";



#ifndef CONFIG_FUNNEL_CHECK_INTERVAL_MIN
#define CONFIG_FUNNEL_CHECK_INTERVAL_MIN 30
#endif
#ifndef CONFIG_FUNNEL_PUBLIC_IPS
#define CONFIG_FUNNEL_PUBLIC_IPS "209.177.145.97,209.177.145.192"
#endif
#ifndef CONFIG_FUNNEL_REPAIR_ENABLED
#define CONFIG_FUNNEL_REPAIR_ENABLED 1
#endif




#define FUNNEL_REPAIR_OFF_HOLD_S 15

#define FUNNEL_REPAIR_SETTLE_S 45


#define FUNNEL_FAILS_BEFORE_REPAIR 2

#define FUNNEL_CHECK_TIMEOUT_MS 9000

#define FUNNEL_FIRST_CHECK_DELAY_S 120



static funnel_ctl_status_t s_st;
static TaskHandle_t s_task = NULL;
static volatile bool s_busy = false;   
static uint32_t s_last_check_at = 0;  
static uint32_t s_last_ok_at = 0;     

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}



static void parse_ips(void) {
    static const char *raw = CONFIG_FUNNEL_PUBLIC_IPS;
    char buf[96];
    snprintf(buf, sizeof(buf), "%s", raw);
    s_st.ip_count = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",; ", &save); tok && s_st.ip_count < FUNNEL_CTL_MAX_IPS;
         tok = strtok_r(NULL, ",; ", &save)) {
        if (strlen(tok) >= FUNNEL_CTL_IPTEXT_LEN) continue;
        snprintf(s_st.ip[s_st.ip_count], FUNNEL_CTL_IPTEXT_LEN, "%s", tok);
        s_st.ip_count++;
    }
    ESP_LOGI(TAG, "IPs publicas del funnel: %u configuradas", s_st.ip_count);
    for (uint8_t i = 0; i < s_st.ip_count; i++) {
        ESP_LOGI(TAG, "  funnel edge ip[%u] = %s", i, s_st.ip[i]);
    }
}













static bool probe_ip(const char *ip, const char *fqdn, char *detail, size_t detail_len) {
    char url[128];
    snprintf(url, sizeof(url), "https://%s/status", ip);

    esp_http_client_config_t cfg = {
        .url = url,
        .common_name = fqdn,          
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .timeout_ms = FUNNEL_CHECK_TIMEOUT_MS,
        .keep_alive_enable = false,
        .disable_auto_redirect = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        snprintf(detail, detail_len, "http_client_init fallo");
        return false;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        snprintf(detail, detail_len, "%s -> %s", ip, esp_err_to_name(err));
        return false;
    }
    if (status < 200 || status >= 500) {
        snprintf(detail, detail_len, "%s -> HTTP %d", ip, status);
        return false;
    }
    snprintf(detail, detail_len, "%s -> HTTP %d (camino publico OK)", ip, status);
    return true;
}

bool funnel_ctl_check_now(char *detail, size_t detail_len) {
    char local[96] = "";
    if (!detail) detail = local;
    if (!detail_len) detail_len = sizeof(local);

    if (s_st.ip_count == 0) {
        snprintf(detail, detail_len, "sin IPs publicas configuradas");
        return false;
    }

    const char *fqdn = microlink_wrapper_get_fqdn();
    if (!fqdn || !fqdn[0]) {
        snprintf(detail, detail_len, "sin FQDN (Tailscale no registrado)");
        return false;
    }

    

    bool present = false;
    size_t cert_len = 0, key_len = 0;
    if (ml_cert_info(&present, &cert_len, &key_len) != ESP_OK || !present) {
        sockmgr_give();
        snprintf(detail, detail_len, "sin cert TLS en NVS: no se puede verificar");
        return false;
    }

    

    if (!sockmgr_take(3000)) {
        snprintf(detail, detail_len, "presupuesto de sockets lleno");
        return false;
    }

    char last[96] = "";
    for (uint8_t i = 0; i < s_st.ip_count; i++) {
        char one[96] = "";
        if (probe_ip(s_st.ip[i], fqdn, one, sizeof(one))) {
            sockmgr_give();
            snprintf(detail, detail_len, "%s", one);
            return true;
        }
        snprintf(last, sizeof(last), "%s", one);
        ESP_LOGW(TAG, "health check fallo: %s", one);
    }
    sockmgr_give();
    snprintf(detail, detail_len, "%s", last[0] ? last : "sin respuesta");
    return false;
}



esp_err_t funnel_ctl_set_enabled(bool on) {
    esp_err_t err = microlink_wrapper_set_funnel_enabled(on);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no se pudo cambiar el anuncio de funnel a %d: %s",
                 on ? 1 : 0, esp_err_to_name(err));
        return err;
    }
    s_st.enabled = on;
    ESP_LOGW(TAG, "anuncio de funnel -> %s (Hostinfo re-anunciado)",
             on ? "ACTIVADO" : "APAGADO");
    return ESP_OK;
}





static volatile bool s_refresh_requested = false;

esp_err_t funnel_ctl_refresh(void) {
    s_refresh_requested = true;
    ESP_LOGW(TAG, "reparacion del funnel solicitada (ciclo off -> on en "
                  "segundo plano)");
    if (s_task) xTaskNotifyGive(s_task);
    return ESP_OK;
}


static esp_err_t funnel_ctl_do_refresh(void) {
    ESP_LOGW(TAG, "=== repairing funnel: ciclo off -> on ===");
    funnel_ctl_set_enabled(false);
    vTaskDelay(pdMS_TO_TICKS(FUNNEL_REPAIR_OFF_HOLD_S * 1000));
    esp_err_t err = funnel_ctl_set_enabled(true);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(FUNNEL_REPAIR_SETTLE_S * 1000));
    s_st.repairs++;
    ESP_LOGW(TAG, "=== repairing funnel: ciclo off -> on completado ===");
    return ESP_OK;
}



static void notify(const char *msg) {
    ESP_LOGW(TAG, "%s", msg);
    telegram_broadcast(msg);
}

static void funnel_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(FUNNEL_FIRST_CHECK_DELAY_S * 1000));

    while (1) {
        
        if (s_refresh_requested) {
            s_refresh_requested = false;
            s_busy = true;
            funnel_ctl_do_refresh();
            s_busy = false;
            char detail2[96] = "";
            bool ok2 = funnel_ctl_check_now(detail2, sizeof(detail2));
            s_st.checks++;
            if (ok2) {
                s_st.healthy = true;
                s_st.consecutive_failures = 0;
                snprintf(s_st.last_error, sizeof(s_st.last_error), "%s", detail2);
                notify("Funnel publico RECUPERADO. El acceso por "
                       "https://<nodo>.tailnet.ts.net vuelve a funcionar.");
            } else {
                snprintf(s_st.last_error, sizeof(s_st.last_error), "%s", detail2);
                notify("Funnel publico SIGUE CAIDO tras la reparacion "
                       "(revisa el cert TLS y la red).");
            }
            continue;
        }

        bool skip = false;
        const char *skip_reason = NULL;

        if (!microlink_wrapper_is_connected()) {
            skip = true; skip_reason = "Tailscale no conectado";
        } else if (!wifi_is_connected()) {
            skip = true; skip_reason = "WiFi caido";
        } else if (!s_st.enabled) {
            skip = true; skip_reason = "funnel apagado a mano";
        }

        if (skip) {
            ESP_LOGD(TAG, "health check omitido: %s", skip_reason);
        } else {
            char detail[96] = "";
            bool ok = false;
            s_busy = true;
            ok = funnel_ctl_check_now(detail, sizeof(detail));
            s_busy = false;

            s_st.checks++;
            s_last_check_at = now_ms();
            s_st.last_check_ms_ago = 0;
            if (ok) {
                s_st.failures = 0;
                s_st.consecutive_failures = 0;
                s_st.healthy = true;
                s_st.ever_ok = true;
                s_last_ok_at = now_ms();
                s_st.last_ok_ms_ago = 0;
                snprintf(s_st.last_error, sizeof(s_st.last_error), "%s", detail);
                ESP_LOGI(TAG, "health check OK: %s", detail);
            } else {
                s_st.failures++;
                s_st.consecutive_failures++;
                s_st.healthy = false;
                snprintf(s_st.last_error, sizeof(s_st.last_error), "%s", detail);
                ESP_LOGE(TAG, "health check FALLIDO: %s", detail);

                


                if (s_st.ever_ok && s_st.consecutive_failures >= FUNNEL_FAILS_BEFORE_REPAIR
                    && CONFIG_FUNNEL_REPAIR_ENABLED) {
                    char msg[220];
                    snprintf(msg, sizeof(msg),
                             "Funnel publico CAIDO (%s). Reparando: apagando y "
                             "volviendo a anunciar el funnel al control plane.",
                             detail);
                    notify(msg);
                    s_busy = true;
                    funnel_ctl_do_refresh();
                    s_busy = false;

                    char detail2[96] = "";
                    bool ok2 = funnel_ctl_check_now(detail2, sizeof(detail2));
                    s_st.checks++;
                    if (ok2) {
                        s_st.healthy = true;
                        s_st.consecutive_failures = 0;
                        snprintf(s_st.last_error, sizeof(s_st.last_error), "%s", detail2);
                        notify("Funnel publico RECUPERADO. El acceso por "
                               "https://<nodo>.tailnet.ts.net vuelve a funcionar.");
                    } else {
                        snprintf(s_st.last_error, sizeof(s_st.last_error), "%s", detail2);
                        notify("Funnel publico SIGUE CAIDO tras la reparacion "
                               "(revisa el cert TLS y la red).");
                    }
                }
            }
        }

        
        for (int i = 0; i < CONFIG_FUNNEL_CHECK_INTERVAL_MIN * 60; i++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

esp_err_t funnel_ctl_init(void) {
    if (s_task != NULL) return ESP_OK;

    parse_ips();
    s_st.enabled = microlink_wrapper_get_funnel_enabled();
    s_st.healthy = false;
    s_st.ever_ok = false;
    snprintf(s_st.last_error, sizeof(s_st.last_error), "sin verificar");

    BaseType_t ok = xTaskCreate(funnel_task, "funnel_ctl", 6144, NULL, 4, &s_task);
    if (ok != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "no se pudo crear la task de supervision del funnel");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "supervision del funnel lista (check cada %u min, ips=%u)",
             (unsigned)CONFIG_FUNNEL_CHECK_INTERVAL_MIN, s_st.ip_count);
    return ESP_OK;
}

void funnel_ctl_get_status(funnel_ctl_status_t *out) {
    if (!out) return;
    *out = s_st;
    uint32_t now = now_ms();
    if (s_last_check_at && now >= s_last_check_at) {
        out->last_check_ms_ago = now - s_last_check_at;
    }
    if (s_last_ok_at && now >= s_last_ok_at) {
        out->last_ok_ms_ago = now - s_last_ok_at;
    }
}

void funnel_ctl_format_status(char *buf, size_t len) {
    if (!buf || len == 0) return;
    snprintf(buf, len,
             "funnel: %s | salud: %s | checks: %u (fallos %u) | reparaciones: %u | %s",
             s_st.enabled ? "anunciado" : "APAGADO",
             s_st.healthy ? "OK" : "SIN VERIFICAR",
             (unsigned)s_st.checks, (unsigned)s_st.failures,
             (unsigned)s_st.repairs,
             s_st.last_error[0] ? s_st.last_error : "-");
}
