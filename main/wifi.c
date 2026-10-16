#include "wifi.h"

#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_sntp.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#include "system_health.h"
#include "sdkconfig.h"

static const char *TAG = "wifi";

EventGroupHandle_t wifi_event_group = NULL;








#define WIFI_KEEPALIVE_INTERVAL_S 5


#define WIFI_STALE_ASSOC_TIMEOUT_S 60

static void wifi_keepalive_task(void *arg) {
    uint32_t assoc_since = 0;
    while (1) {
        system_health_heartbeat(HEALTH_CHANNEL_WIFI);
        vTaskDelay(pdMS_TO_TICKS(WIFI_KEEPALIVE_INTERVAL_S * 1000));
        system_health_heartbeat(HEALTH_CHANNEL_WIFI);
        if (wifi_event_group == NULL) continue;

        bool have_ip = (xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
        if (have_ip) {
            assoc_since = 0;
            continue;
        }

        wifi_ap_record_t ap;
        esp_err_t aerr = esp_wifi_sta_get_ap_info(&ap);
        if (aerr != ESP_OK) {
            



            if (assoc_since != 0) assoc_since = 0;
            esp_err_t cerr = esp_wifi_connect();
            if (cerr != ESP_OK && cerr != ESP_ERR_WIFI_CONN) {
                ESP_LOGW(TAG, "keepalive: esp_wifi_connect() -> %s",
                         esp_err_to_name(cerr));
            }
            continue;
        }

        

        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (assoc_since == 0) assoc_since = now;
        if ((now - assoc_since) >= (WIFI_STALE_ASSOC_TIMEOUT_S * 1000U)) {
            ESP_LOGW(TAG, "keepalive: asociado pero sin IP en %d s — re-asociando",
                     WIFI_STALE_ASSOC_TIMEOUT_S);
            esp_wifi_disconnect();
            assoc_since = 0;
        }
    }
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        


        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        if (event_data != NULL) {
            wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)event_data;
            ESP_LOGW(TAG, "WiFi DISCONNECTED (reason=%d) — keepalive reconnecting...",
                     (int)d->reason);
        } else {
            ESP_LOGW(TAG, "WiFi DISCONNECTED — keepalive reconnecting...");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        if (esp_sntp_enabled()) {
            ESP_LOGI(TAG, "restarting SNTP for immediate clock sync");
            esp_sntp_restart();
        }
    }
}

esp_err_t wifi_init_sta(void) {
    wifi_event_group = xEventGroupCreate();
    if (wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_WIFI_SSID,
            .password = CONFIG_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished, connecting to SSID: %s", CONFIG_WIFI_SSID);

    


    BaseType_t ok = xTaskCreate(wifi_keepalive_task, "wifi_keepalive", 4096, NULL, 2, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(wifi_keepalive) failed");
    }

    

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(CONFIG_WIFI_CONNECT_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to SSID: %s", CONFIG_WIFI_SSID);
        return ESP_OK;
    }
    ESP_LOGW(TAG, "wifi init sin conexión tras %u ms — keepalive sigue reintentando en background",
             (unsigned)CONFIG_WIFI_CONNECT_TIMEOUT_MS);
    return ESP_ERR_TIMEOUT;
}

bool wifi_is_connected(void) {
    if (wifi_event_group == NULL) return false;
    EventBits_t bits = xEventGroupGetBits(wifi_event_group);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

esp_ip4_addr_t wifi_get_ip(void) {
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip_info) == ESP_OK) {
        return ip_info.ip;
    }
    esp_ip4_addr_t zero = {0};
    return zero;
}
