#include "microlink_wrapper.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#include "microlink.h"
#include "microlink_internal.h"

#include "sdkconfig.h"

static const char *TAG = "microlink";

static microlink_t *s_ml = NULL;





static volatile uint32_t s_last_connected_ms = 0;

static uint32_t now_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static const char *state_name(microlink_state_t s) {
    switch (s) {
        case ML_STATE_IDLE:         return "IDLE";
        case ML_STATE_WIFI_WAIT:    return "WIFI_WAIT";
        case ML_STATE_CONNECTING:   return "CONNECTING";
        case ML_STATE_REGISTERING:  return "REGISTERING";
        case ML_STATE_CONNECTED:    return "CONNECTED";
        case ML_STATE_RECONNECTING: return "RECONNECTING";
        case ML_STATE_ERROR:        return "ERROR";
        default:                    return "?";
    }
}

static void on_state_change(microlink_t *ml, microlink_state_t state, void *user_data) {
    ESP_LOGI(TAG, "state -> %s", state_name(state));
    if (state == ML_STATE_CONNECTED) {
        uint32_t ip = microlink_get_vpn_ip(ml);
        char ip_str[16];
        microlink_ip_to_str(ip, ip_str);
        ESP_LOGI(TAG, "*** TAILSCALE CONNECTED *** VPN IP: %s", ip_str);
        s_last_connected_ms = now_ms();
    }
}

esp_err_t microlink_wrapper_start(void) {
    if (s_ml != NULL) {
        ESP_LOGW(TAG, "already started");
        return ESP_OK;
    }

#if CONFIG_TAILSCALE_ENABLED
    microlink_config_t config = {
        .auth_key = CONFIG_TAILSCALE_AUTH_KEY,
        .device_name = (strlen(CONFIG_TAILSCALE_DEVICE_NAME) > 0)
                       ? CONFIG_TAILSCALE_DEVICE_NAME
                       : microlink_default_device_name(),
        .enable_derp = true,
        .enable_stun = true,
        .enable_disco = true,
        





        .max_peers = 40,
        .wifi_tx_power_dbm = 13,
#if CONFIG_TAILSCALE_FUNNEL_ENABLED
        .enable_funnel = true,
        .peerapi_port = CONFIG_TAILSCALE_PEERAPI_PORT,
#else
        .enable_funnel = false,
        .peerapi_port = 0,
#endif
    };

    if (config.auth_key == NULL || strlen(config.auth_key) == 0) {
        ESP_LOGE(TAG, "TAILSCALE_AUTH_KEY is empty. Set via menuconfig or sdkconfig.credentials.");
        return ESP_ERR_INVALID_ARG;
    }

    s_ml = microlink_init(&config);
    if (s_ml == NULL) {
        ESP_LOGE(TAG, "microlink_init failed");
        return ESP_FAIL;
    }

    microlink_set_state_callback(s_ml, on_state_change, NULL);
    esp_err_t err = microlink_start(s_ml);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "microlink_start failed: %s", esp_err_to_name(err));
        microlink_destroy(s_ml);
        s_ml = NULL;
        return err;
    }
    ESP_LOGI(TAG, "microlink started; connecting to Tailscale...");
    return ESP_OK;
#else
    ESP_LOGW(TAG, "Tailscale disabled via menuconfig (CONFIG_TAILSCALE_ENABLED=n)");
    return ESP_OK;
#endif
}

esp_err_t microlink_wrapper_stop(void) {
    if (s_ml == NULL) return ESP_OK;
    esp_err_t err = microlink_stop(s_ml);
    microlink_destroy(s_ml);
    s_ml = NULL;
    return err;
}

bool microlink_wrapper_is_connected(void) {
    return (s_ml != NULL) && microlink_is_connected(s_ml);
}

uint32_t microlink_wrapper_get_vpn_ip(void) {
    return (s_ml != NULL) ? microlink_get_vpn_ip(s_ml) : 0;
}

esp_err_t microlink_wrapper_trigger_handshake(uint32_t dest_vpn_ip) {
    if (s_ml == NULL) return ESP_ERR_INVALID_STATE;
    return ml_wg_mgr_trigger_handshake(s_ml, dest_vpn_ip);
}

esp_err_t microlink_wrapper_send_cmm(uint32_t dest_vpn_ip) {
    if (s_ml == NULL) return ESP_ERR_INVALID_STATE;
    ml_wg_mgr_send_cmm(s_ml, dest_vpn_ip);
    return ESP_OK;
}

bool microlink_wrapper_peer_is_up(uint32_t dest_vpn_ip) {
    if (s_ml == NULL) return false;
    return ml_wg_mgr_peer_is_up(s_ml, dest_vpn_ip);
}



uint32_t microlink_wrapper_get_last_connected_ms(void) {
    return s_last_connected_ms;
}




uint32_t microlink_wrapper_get_ctrl_stale_ms(void) {
    return (s_ml != NULL) ? microlink_get_ctrl_stale_ms(s_ml) : 0;
}


const char *microlink_wrapper_state_name(microlink_state_t s) {
    return state_name(s);
}

microlink_state_t microlink_wrapper_get_state(void) {
    if (s_ml == NULL) return ML_STATE_IDLE;
    return microlink_get_state(s_ml);
}

esp_err_t microlink_wrapper_rebind(void) {
    if (s_ml == NULL) return ESP_ERR_INVALID_STATE;
    microlink_state_t st = microlink_get_state(s_ml);
    if (st == ML_STATE_IDLE) return ESP_ERR_INVALID_STATE;
    ESP_LOGW(TAG, ">>> rebind() triggered (was %s)", state_name(st));
    esp_err_t err = microlink_rebind(s_ml);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rebind failed: %s", esp_err_to_name(err));
    }
    return err;
}
const char *microlink_wrapper_get_fqdn(void) {
    if (s_ml == NULL) return NULL;
    return microlink_get_fqdn(s_ml);
}

esp_err_t microlink_wrapper_set_dns(const char *name, const char *value,
                                    uint32_t timeout_ms) {
    return microlink_set_dns(name, value, timeout_ms);
}
