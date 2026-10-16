













#include "ml_cellular.h"
#include "microlink_internal.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "lwip/ip6_addr.h"
#include "esp_event.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef CONFIG_ML_ENABLE_CELLULAR

static const char *TAG = "ml_cell";





#define UART_NUM            UART_NUM_1
#define UART_RX_BUF_SIZE    (16 * 1024)  
#define UART_TX_BUF_SIZE    (4 * 1024)   
#define AT_TIMEOUT_MS       5000
#define AT_SHORT_TIMEOUT_MS 2000
#define PPP_CONNECT_TIMEOUT_MS 30000


#define PPP_GOT_IP_BIT      BIT0
#define PPP_LOST_IP_BIT     BIT1
#define PPP_CONNECT_FAIL    BIT2
#define PPP_CHAP_FAIL_BIT   BIT3
#define PPP_CLOSED_BIT      BIT4   
#define PPP_GOT_IP6_BIT     BIT5

static struct {
    ml_cellular_config_t config;
    volatile ml_cellular_state_t state;
    ml_cellular_info_t info;
    bool uart_installed;
    ml_cellular_data_mode_t data_mode;

    
    esp_netif_t *ppp_netif;
    TaskHandle_t ppp_task;
    EventGroupHandle_t ppp_events;
    volatile bool ppp_running;
    esp_event_handler_instance_t ip_event_handler;
    esp_event_handler_instance_t ppp_status_handler;
} s_cell = {
    .state = ML_CELL_STATE_OFF,
    .data_mode = ML_DATA_MODE_NONE,
};





static int at_send_cmd(const char *cmd, char *response, size_t resp_size, int timeout_ms)
{
    uart_flush_input(UART_NUM);

    char cmd_buf[256];
    snprintf(cmd_buf, sizeof(cmd_buf), "%s\r\n", cmd);
    uart_write_bytes(UART_NUM, cmd_buf, strlen(cmd_buf));

    ESP_LOGD(TAG, ">> %s", cmd);

    int total_read = 0;
    int64_t start = esp_timer_get_time();
    int64_t timeout_us = (int64_t)timeout_ms * 1000;

    memset(response, 0, resp_size);

    while ((esp_timer_get_time() - start) < timeout_us) {
        int len = uart_read_bytes(UART_NUM,
                                   (uint8_t *)(response + total_read),
                                   resp_size - total_read - 1,
                                   pdMS_TO_TICKS(100));
        if (len > 0) {
            total_read += len;
            response[total_read] = '\0';

            if (strstr(response, "OK") ||
                strstr(response, "ERROR") ||
                strstr(response, "+CME ERROR") ||
                strstr(response, "+CMS ERROR") ||
                strstr(response, "CONNECT")) {
                break;
            }
        }
    }

    if (total_read > 0) {
        char *display = response;
        while (*display == '\r' || *display == '\n') display++;
        ESP_LOGD(TAG, "<< %s", display);
    } else {
        ESP_LOGW(TAG, "<< (no response to '%s')", cmd);
        return -1;
    }

    return total_read;
}

static bool at_cmd_ok(const char *cmd, int timeout_ms)
{
    char resp[512];
    int len = at_send_cmd(cmd, resp, sizeof(resp), timeout_ms);
    return (len > 0 && strstr(resp, "OK") != NULL);
}


static bool at_extract_value(const char *resp, const char *prefix, char *out, size_t out_size)
{
    const char *p = NULL;
    if (prefix && prefix[0]) {
        p = strstr(resp, prefix);
    }
    if (!p) {
        
        p = resp;
        while (*p == '\r' || *p == '\n' || *p == ' ') p++;
    } else {
        p += strlen(prefix);
        while (*p == ' ' || *p == ':') p++;
    }
    const char *end = p;
    while (*end && *end != '\r' && *end != '\n') end++;
    size_t len = (size_t)(end - p);
    if (len == 0 || len >= out_size) return false;
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}





static esp_err_t uart_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = s_cell.config.baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err;

    err = uart_driver_install(UART_NUM, UART_RX_BUF_SIZE, UART_TX_BUF_SIZE, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART driver install failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_param_config(UART_NUM, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART param config failed: %s", esp_err_to_name(err));
        uart_driver_delete(UART_NUM);
        return err;
    }

    err = uart_set_pin(UART_NUM, s_cell.config.tx_pin, s_cell.config.rx_pin,
                        UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART set pin failed: %s", esp_err_to_name(err));
        uart_driver_delete(UART_NUM);
        return err;
    }

    
    uart_set_rx_timeout(UART_NUM, 1);

    s_cell.uart_installed = true;
    ESP_LOGI(TAG, "UART%d: TX=GPIO%d, RX=GPIO%d, Baud=%d",
             UART_NUM, s_cell.config.tx_pin, s_cell.config.rx_pin, s_cell.config.baud_rate);

    return ESP_OK;
}





static esp_err_t modem_wait_ready(void)
{
    ESP_LOGI(TAG, "Waiting for modem...");

    


    vTaskDelay(pdMS_TO_TICKS(1100));
    uart_write_bytes(UART_NUM, "+++", 3);
    vTaskDelay(pdMS_TO_TICKS(1100));
    uart_flush_input(UART_NUM);

    
    for (int i = 0; i < 10; i++) {
        if (at_cmd_ok("AT", AT_SHORT_TIMEOUT_MS)) {
            s_cell.state = ML_CELL_STATE_AT_OK;
            ESP_LOGI(TAG, "Modem responding at %d baud", s_cell.config.baud_rate);
            return ESP_OK;
        }
        

        if (i == 3) {
#ifdef CONFIG_ML_BOARD_LILYGO_T_SIM7670G
            
            int try_bauds[] = {115200};
            int num_bauds = 1;
#else
            int try_bauds[] = {921600, 460800, 115200};
            int num_bauds = 3;
#endif
            for (int b = 0; b < num_bauds; b++) {
                if (try_bauds[b] == s_cell.config.baud_rate) continue;
                ESP_LOGI(TAG, "Trying alternate baud rate %d...", try_bauds[b]);
                uart_set_baudrate(UART_NUM, try_bauds[b]);
                vTaskDelay(pdMS_TO_TICKS(100));
                
                vTaskDelay(pdMS_TO_TICKS(1100));
                uart_write_bytes(UART_NUM, "+++", 3);
                vTaskDelay(pdMS_TO_TICKS(1100));
                uart_flush_input(UART_NUM);
                if (at_cmd_ok("AT", AT_SHORT_TIMEOUT_MS)) {
                    s_cell.config.baud_rate = try_bauds[b];
                    s_cell.state = ML_CELL_STATE_AT_OK;
                    ESP_LOGI(TAG, "Modem responding at %d baud", try_bauds[b]);
                    return ESP_OK;
                }
            }
            
            uart_set_baudrate(UART_NUM, s_cell.config.baud_rate);
        }
        ESP_LOGW(TAG, "AT retry %d/10...", i + 1);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    ESP_LOGE(TAG, "Modem not responding after 10 retries");
    s_cell.state = ML_CELL_STATE_ERROR;
    return ESP_ERR_TIMEOUT;
}

static esp_err_t modem_setup(void)
{
    char resp[512];

    
    at_cmd_ok("ATE0", AT_SHORT_TIMEOUT_MS);

    
    at_send_cmd("AT+CGMM", resp, sizeof(resp), AT_SHORT_TIMEOUT_MS);
    at_extract_value(resp, NULL, s_cell.info.model, sizeof(s_cell.info.model));
    ESP_LOGI(TAG, "Model: %s", s_cell.info.model);

    
    at_send_cmd("AT+CGMR", resp, sizeof(resp), AT_SHORT_TIMEOUT_MS);
    at_extract_value(resp, NULL, s_cell.info.firmware, sizeof(s_cell.info.firmware));

    
    at_send_cmd("AT+CGSN", resp, sizeof(resp), AT_SHORT_TIMEOUT_MS);
    at_extract_value(resp, NULL, s_cell.info.imei, sizeof(s_cell.info.imei));
    
    bool valid_imei = (strlen(s_cell.info.imei) >= 14);
    for (size_t i = 0; i < strlen(s_cell.info.imei) && valid_imei; i++) {
        if (s_cell.info.imei[i] < '0' || s_cell.info.imei[i] > '9') valid_imei = false;
    }
    if (valid_imei) {
        ESP_LOGI(TAG, "IMEI: %s", s_cell.info.imei);
    } else {
        ESP_LOGW(TAG, "IMEI invalid: '%s'", s_cell.info.imei);
        s_cell.info.imei[0] = '\0';
    }

    return ESP_OK;
}

static esp_err_t sim_setup(void)
{
    char resp[512];

    
    at_send_cmd("AT+CPIN?", resp, sizeof(resp), AT_SHORT_TIMEOUT_MS);

    if (strstr(resp, "READY")) {
        s_cell.info.sim_ready = true;
        s_cell.state = ML_CELL_STATE_SIM_READY;
        ESP_LOGI(TAG, "SIM ready");
    } else if (strstr(resp, "SIM PIN")) {
        
        if (s_cell.config.sim_pin && s_cell.config.sim_pin[0]) {
            char pin_cmd[64];
            snprintf(pin_cmd, sizeof(pin_cmd), "AT+CPIN=\"%s\"", s_cell.config.sim_pin);
            if (at_cmd_ok(pin_cmd, AT_TIMEOUT_MS)) {
                ESP_LOGI(TAG, "SIM PIN accepted");
                vTaskDelay(pdMS_TO_TICKS(2000)); 

                
                at_send_cmd("AT+CPIN?", resp, sizeof(resp), AT_SHORT_TIMEOUT_MS);
                if (strstr(resp, "READY")) {
                    s_cell.info.sim_ready = true;
                    s_cell.state = ML_CELL_STATE_SIM_READY;
                } else {
                    ESP_LOGE(TAG, "SIM still not ready after PIN");
                    s_cell.state = ML_CELL_STATE_ERROR;
                    return ESP_ERR_INVALID_STATE;
                }
            } else {
                ESP_LOGE(TAG, "SIM PIN rejected");
                s_cell.state = ML_CELL_STATE_ERROR;
                return ESP_ERR_INVALID_ARG;
            }
        } else {
            ESP_LOGE(TAG, "SIM requires PIN but none configured");
            s_cell.state = ML_CELL_STATE_ERROR;
            return ESP_ERR_INVALID_STATE;
        }
    } else {
        ESP_LOGE(TAG, "SIM not detected — check SIM card insertion");
        s_cell.state = ML_CELL_STATE_ERROR;
        return ESP_ERR_NOT_FOUND;
    }

    
    at_send_cmd("AT+CICCID", resp, sizeof(resp), AT_SHORT_TIMEOUT_MS);
    at_extract_value(resp, "+ICCID:", s_cell.info.iccid, sizeof(s_cell.info.iccid));
    if (s_cell.info.iccid[0]) {
        ESP_LOGI(TAG, "ICCID: %s", s_cell.info.iccid);
    }

    return ESP_OK;
}

static bool check_registration(const char *resp)
{
    
    return strstr(resp, ",1") != NULL || strstr(resp, ",5") != NULL;
}

static esp_err_t network_register(void)
{
    char resp[512];

    ESP_LOGI(TAG, "Waiting for network registration...");

    



    for (int i = 0; i < 30; i++) {
        bool registered = false;

        at_send_cmd("AT+CREG?", resp, sizeof(resp), AT_SHORT_TIMEOUT_MS);
        if (check_registration(resp)) {
            registered = true;
            ESP_LOGI(TAG, "CS network registered");
        }

        at_send_cmd("AT+CEREG?", resp, sizeof(resp), AT_SHORT_TIMEOUT_MS);
        if (check_registration(resp)) {
            registered = true;
            ESP_LOGI(TAG, "EPS/LTE network registered");
        } else if (strstr(resp, ",6") != NULL) {
            

            ESP_LOGW(TAG, "EPS registered for SMS only (stat=6) — no data service. "
                     "Check SIM/carrier/APN or update modem firmware.");
        }

        if (registered) {
            s_cell.info.registered = true;
            s_cell.state = ML_CELL_STATE_REGISTERED;
            ESP_LOGI(TAG, "Network registered");

            
            at_send_cmd("AT+COPS?", resp, sizeof(resp), AT_TIMEOUT_MS);
            char *op_start = strstr(resp, "\"");
            if (op_start) {
                op_start++;
                char *op_end = strstr(op_start, "\"");
                if (op_end) {
                    size_t len = (size_t)(op_end - op_start);
                    if (len < sizeof(s_cell.info.operator_name)) {
                        memcpy(s_cell.info.operator_name, op_start, len);
                        s_cell.info.operator_name[len] = '\0';
                        ESP_LOGI(TAG, "Operator: %s", s_cell.info.operator_name);
                    }
                }
            }

            
            at_send_cmd("AT+CSQ", resp, sizeof(resp), AT_SHORT_TIMEOUT_MS);
            char *csq = strstr(resp, "+CSQ:");
            if (csq) {
                int rssi = 0, ber = 0;
                sscanf(csq, "+CSQ: %d,%d", &rssi, &ber);
                s_cell.info.rssi = rssi;
                s_cell.info.rssi_dbm = (rssi == 99) ? 0 : (-113 + rssi * 2);
                ESP_LOGI(TAG, "Signal: RSSI=%d (%d dBm)", rssi, s_cell.info.rssi_dbm);
            }

            return ESP_OK;
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    ESP_LOGE(TAG, "Network registration timeout (60s)");
    s_cell.state = ML_CELL_STATE_ERROR;
    return ESP_ERR_TIMEOUT;
}






static uint32_t s_ppp_tx_count = 0;

static esp_err_t ppp_transmit(void *h, void *buffer, size_t len)
{
    s_ppp_tx_count++;
    if (s_ppp_tx_count <= 10 || (s_ppp_tx_count % 20) == 0) {
        ESP_LOGI(TAG, "PPP TX: %d bytes (frame #%lu)", (int)len, (unsigned long)s_ppp_tx_count);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, buffer, len < 64 ? len : 64, ESP_LOG_INFO);
    }
    uart_write_bytes(UART_NUM, buffer, len);
    return ESP_OK;
}

static esp_netif_driver_ifconfig_t s_ppp_driver_cfg = {
    .handle = (void *)1,   
    .transmit = ppp_transmit,
};



static void ppp_status_event_handler(void *arg, esp_event_base_t event_base,
                                       int32_t event_id, void *event_data)
{
    if (event_id == NETIF_PPP_ERRORUSER) {
        
        ESP_LOGI(TAG, "PPP closed (user interrupt)");
        if (s_cell.ppp_events) {
            xEventGroupSetBits(s_cell.ppp_events, PPP_CLOSED_BIT);
        }
    } else if (event_id == NETIF_PPP_ERRORAUTHFAIL) {
        ESP_LOGW(TAG, "PPP CHAP authentication failed — will fall back to AT");
        if (s_cell.ppp_events) {
            xEventGroupSetBits(s_cell.ppp_events, PPP_CHAP_FAIL_BIT);
        }
    } else if (event_id == NETIF_PPP_ERRORCONNECT) {
        ESP_LOGW(TAG, "PPP connection error");
        if (s_cell.ppp_events) {
            xEventGroupSetBits(s_cell.ppp_events, PPP_CONNECT_FAIL);
        }
    } else if (event_id < NETIF_PP_PHASE_OFFSET && event_id != NETIF_PPP_ERRORNONE) {
        ESP_LOGW(TAG, "PPP error event: %ld", (long)event_id);
        if (s_cell.ppp_events) {
            xEventGroupSetBits(s_cell.ppp_events, PPP_CONNECT_FAIL);
        }
    }
}


static void ppp_ip_event_handler(void *arg, esp_event_base_t event_base,
                                   int32_t event_id, void *event_data)
{
    if (event_id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        if (event->esp_netif != s_cell.ppp_netif) return;

        ESP_LOGI(TAG, "PPP got IP: " IPSTR, IP2STR(&event->ip_info.ip));

        esp_netif_dns_info_t dns_info;
        esp_netif_get_dns_info(s_cell.ppp_netif, ESP_NETIF_DNS_MAIN, &dns_info);
        ESP_LOGI(TAG, "PPP DNS: " IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));

        s_cell.info.data_connected = true;
        s_cell.state = ML_CELL_STATE_PPP_CONNECTED;
        xEventGroupSetBits(s_cell.ppp_events, PPP_GOT_IP_BIT);
    } else if (event_id == IP_EVENT_GOT_IP6) {
        ip_event_got_ip6_t *event = (ip_event_got_ip6_t *)event_data;
        if (event->esp_netif != s_cell.ppp_netif) return;

        
        if (ip6_addr_islinklocal(&event->ip6_info.ip)) {
            ESP_LOGI(TAG, "PPP got link-local IPv6 (waiting for global)");
            return;
        }

        ESP_LOGI(TAG, "PPP got IPv6: " IPV6STR, IPV62STR(event->ip6_info.ip));
        s_cell.info.data_connected = true;
        s_cell.state = ML_CELL_STATE_PPP_CONNECTED;
        xEventGroupSetBits(s_cell.ppp_events, PPP_GOT_IP6_BIT);
    } else if (event_id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGW(TAG, "PPP lost IP");
        s_cell.info.data_connected = false;
        s_cell.state = ML_CELL_STATE_REGISTERED;
        xEventGroupSetBits(s_cell.ppp_events, PPP_LOST_IP_BIT);
    }
}


static void ppp_rx_task(void *arg)
{
    uint8_t *buf = ml_psram_malloc(UART_RX_BUF_SIZE);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate PPP RX buffer");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "PPP RX task started");

    int rx_frame_count = 0;
    while (s_cell.ppp_running) {
        int len = uart_read_bytes(UART_NUM, buf, UART_RX_BUF_SIZE, pdMS_TO_TICKS(100));
        if (len > 0) {
            rx_frame_count++;
            if (rx_frame_count <= 15) {  
                ESP_LOGI(TAG, "PPP RX: %d bytes (chunk #%d)", len, rx_frame_count);
            }
            esp_netif_receive(s_cell.ppp_netif, buf, len, NULL);
        }
    }

    free(buf);
    ESP_LOGI(TAG, "PPP RX task stopped");
    vTaskDelete(NULL);
}

static esp_err_t ppp_setup_and_dial(void)
{
    char resp[512];

    


    at_send_cmd("ATH", resp, sizeof(resp), 5000);
    ESP_LOGI(TAG, "PPP cleanup: ATH -> %.30s", resp);
    vTaskDelay(pdMS_TO_TICKS(500));

    

    at_send_cmd("AT+NETCLOSE", resp, sizeof(resp), 5000);
    ESP_LOGI(TAG, "PPP cleanup: NETCLOSE -> %.30s", resp);
    vTaskDelay(pdMS_TO_TICKS(500));

    



    at_send_cmd("AT+CGACT=0,1", resp, sizeof(resp), 5000);
    ESP_LOGI(TAG, "PPP cleanup: CGACT=0,1 -> %.30s", resp);

    
    at_send_cmd("AT+CGACT?", resp, sizeof(resp), 5000);
    ESP_LOGI(TAG, "PPP cleanup: CGACT? -> %.60s", resp);
    vTaskDelay(pdMS_TO_TICKS(2000));

    



    {
        const char *apn = (s_cell.config.apn && s_cell.config.apn[0])
                          ? s_cell.config.apn : "";
        char cgdcont_cmd[128];
        snprintf(cgdcont_cmd, sizeof(cgdcont_cmd),
                 "AT+CGDCONT=1,\"IP\",\"%s\"", apn);
        at_cmd_ok(cgdcont_cmd, AT_TIMEOUT_MS);
        if (apn[0]) {
            ESP_LOGI(TAG, "PDP context: APN=%s", apn);
        } else {
            ESP_LOGW(TAG, "PDP context: no APN set (using network default) — "
                     "set APN via menuconfig or web UI for reliable PPP");
        }
    }

    



    const char *ppp_user = s_cell.config.ppp_user ? s_cell.config.ppp_user : "";
    const char *ppp_pass = s_cell.config.ppp_pass ? s_cell.config.ppp_pass : "";
    {
        
        int auth_type = (ppp_user[0] || ppp_pass[0]) ? 2 : 1;  
        char auth_cmd[128];
        snprintf(auth_cmd, sizeof(auth_cmd),
                 "AT+CGAUTH=1,%d,\"%s\",\"%s\"", auth_type, ppp_user, ppp_pass);
        at_cmd_ok(auth_cmd, AT_TIMEOUT_MS);
    }
    if (ppp_user[0] || ppp_pass[0]) {
        ESP_LOGI(TAG, "PPP credentials: user=%s (CHAP)", ppp_user);
    } else {
        ESP_LOGI(TAG, "PPP: empty credentials, PAP auth (IMSI-based)");
    }

    



    
    s_cell.ppp_events = xEventGroupCreate();
    if (!s_cell.ppp_events) {
        ESP_LOGE(TAG, "Failed to create PPP event group");
        return ESP_ERR_NO_MEM;
    }

    
    esp_err_t err = esp_event_handler_instance_register(
        IP_EVENT, ESP_EVENT_ANY_ID,
        ppp_ip_event_handler, NULL,
        &s_cell.ip_event_handler);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP event handler: %s", esp_err_to_name(err));
        return err;
    }

    
    esp_netif_inherent_config_t base_cfg = ESP_NETIF_INHERENT_DEFAULT_PPP();
    base_cfg.if_desc = "ml_ppp";
    base_cfg.route_prio = 10;   

    esp_netif_config_t ppp_config = {
        .base = &base_cfg,
        .driver = &s_ppp_driver_cfg,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_PPP,
    };

    s_cell.ppp_netif = esp_netif_new(&ppp_config);
    if (!s_cell.ppp_netif) {
        ESP_LOGE(TAG, "Failed to create PPP netif");
        return ESP_FAIL;
    }

    
    esp_netif_auth_type_t auth = (ppp_user[0] || ppp_pass[0])
                                 ? NETIF_PPP_AUTHTYPE_CHAP
                                 : NETIF_PPP_AUTHTYPE_PAP;
    esp_netif_ppp_set_auth(s_cell.ppp_netif, auth, ppp_user, ppp_pass);

    
    esp_netif_ppp_config_t ppp_evt_cfg = {
        .ppp_phase_event_enabled = false,
        .ppp_error_event_enabled = true,
    };
    esp_netif_ppp_set_params(s_cell.ppp_netif, &ppp_evt_cfg);

    
    esp_event_handler_instance_register(
        NETIF_PPP_STATUS, ESP_EVENT_ANY_ID,
        ppp_status_event_handler, NULL,
        &s_cell.ppp_status_handler);

    ESP_LOGI(TAG, "PPP CHAP auth configured (user=%s)", ppp_user[0] ? ppp_user : "<empty>");

    



    


    ESP_LOGI(TAG, "Dialing PPP (ATD*99***1#)...");
    s_cell.state = ML_CELL_STATE_PPP_CONNECTING;

    bool dial_ok = false;
    for (int attempt = 1; attempt <= 4; attempt++) {
        at_send_cmd("ATD*99***1#", resp, sizeof(resp), 15000);
        if (strstr(resp, "CONNECT")) {
            dial_ok = true;
            break;
        }
        ESP_LOGW(TAG, "PPP dial attempt %d/4 failed: %.30s", attempt, resp);
        if (attempt < 4) {
            



            at_send_cmd("ATH", resp, sizeof(resp), 5000);
            at_send_cmd("AT+CGACT=0,1", resp, sizeof(resp), 5000);
            if (attempt >= 3) {
                
                ESP_LOGW(TAG, "PPP dial: CFUN reset on attempt %d", attempt);
                at_send_cmd("AT+CFUN=0", resp, sizeof(resp), 10000);
                vTaskDelay(pdMS_TO_TICKS(2000));
                at_send_cmd("AT+CFUN=1", resp, sizeof(resp), 10000);
                vTaskDelay(pdMS_TO_TICKS(8000));
                
                at_send_cmd("AT+CEREG?", resp, sizeof(resp), 5000);
                ESP_LOGI(TAG, "Post-CFUN: CEREG -> %.40s", resp);
            } else {
                int delay_ms = (attempt == 1) ? 3000 : 5000;
                vTaskDelay(pdMS_TO_TICKS(delay_ms));
            }
        }
    }
    if (!dial_ok) {
        ESP_LOGE(TAG, "PPP dial failed (3 attempts)");
        s_cell.state = ML_CELL_STATE_ERROR;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "PPP CONNECT received, starting PPP...");

    

    s_cell.ppp_running = true;
    BaseType_t ret = xTaskCreatePinnedToCore(
        ppp_rx_task, "ml_ppp_rx", 4096, NULL,
        5, &s_cell.ppp_task, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create PPP RX task");
        s_cell.ppp_running = false;
        s_cell.state = ML_CELL_STATE_ERROR;
        return ESP_FAIL;
    }

    
    vTaskDelay(pdMS_TO_TICKS(50));

    

    esp_netif_action_start(s_cell.ppp_netif, 0, 0, 0);
    esp_netif_action_connected(s_cell.ppp_netif, 0, 0, 0);

    
    EventBits_t bits = xEventGroupWaitBits(
        s_cell.ppp_events,
        PPP_GOT_IP_BIT | PPP_GOT_IP6_BIT | PPP_CONNECT_FAIL | PPP_CHAP_FAIL_BIT,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(PPP_CONNECT_TIMEOUT_MS));

    if (bits & (PPP_GOT_IP_BIT | PPP_GOT_IP6_BIT)) {
        ESP_LOGI(TAG, "PPP data connection established (%s)",
                 (bits & PPP_GOT_IP_BIT) ? "IPv4" : "IPv6");
        return ESP_OK;
    }

    if (bits & PPP_CHAP_FAIL_BIT) {
        ESP_LOGW(TAG, "PPP CHAP authentication failed");
        s_cell.ppp_running = false;
        vTaskDelay(pdMS_TO_TICKS(200));
        s_cell.state = ML_CELL_STATE_REGISTERED;
        return ESP_ERR_NOT_SUPPORTED;   
    }

    ESP_LOGE(TAG, "PPP connection timeout");
    s_cell.ppp_running = false;
    vTaskDelay(pdMS_TO_TICKS(200)); 
    s_cell.state = ML_CELL_STATE_ERROR;
    return ESP_ERR_TIMEOUT;
}





esp_err_t ml_cellular_init(const ml_cellular_config_t *config)
{
    if (s_cell.state != ML_CELL_STATE_OFF) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    
    if (config) {
        s_cell.config = *config;
    } else {
        ml_cellular_config_t def = ML_CELLULAR_DEFAULT_CONFIG();
        s_cell.config = def;
    }

    if (s_cell.config.tx_pin == 0) s_cell.config.tx_pin = 43;
    if (s_cell.config.rx_pin == 0) s_cell.config.rx_pin = 44;
    if (s_cell.config.baud_rate == 0) s_cell.config.baud_rate = 115200;

    memset(&s_cell.info, 0, sizeof(s_cell.info));
    s_cell.state = ML_CELL_STATE_INIT;

    
    esp_err_t err = uart_init();
    if (err != ESP_OK) return err;

    


#if CONFIG_ML_CELLULAR_PWRKEY_PIN >= 0
    ESP_LOGI(TAG, "Modem power-on: PWRKEY pulse on GPIO%d", CONFIG_ML_CELLULAR_PWRKEY_PIN);
    gpio_config_t pwrkey_conf = {
        .pin_bit_mask = (1ULL << CONFIG_ML_CELLULAR_PWRKEY_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pwrkey_conf);
    gpio_set_level(CONFIG_ML_CELLULAR_PWRKEY_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(CONFIG_ML_CELLULAR_PWRKEY_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(1000));
    gpio_set_level(CONFIG_ML_CELLULAR_PWRKEY_PIN, 1);
#endif
#if CONFIG_ML_CELLULAR_DTR_PIN >= 0
    gpio_config_t dtr_conf = {
        .pin_bit_mask = (1ULL << CONFIG_ML_CELLULAR_DTR_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&dtr_conf);
    gpio_set_level(CONFIG_ML_CELLULAR_DTR_PIN, 0);
#endif

    

#ifdef CONFIG_ML_BOARD_LILYGO_T_SIM7670G
    ESP_LOGI(TAG, "Waiting 5s for SIM7670G boot...");
    vTaskDelay(pdMS_TO_TICKS(5000));
#else
    ESP_LOGI(TAG, "Waiting 3s for modem boot...");
    vTaskDelay(pdMS_TO_TICKS(3000));
#endif

    err = modem_wait_ready();
    if (err != ESP_OK) return err;

    
    err = modem_setup();
    if (err != ESP_OK) return err;

    



#ifdef CONFIG_ML_BOARD_LILYGO_T_SIM7670G
    int fast_baud = 0;  
    ESP_LOGI(TAG, "SIM7670G: keeping baud at 115200 (no HW flow control)");
#else
    int fast_baud = 921600;
#endif
    if (fast_baud && s_cell.config.baud_rate == 115200) {
        char ipr_cmd[32];
        snprintf(ipr_cmd, sizeof(ipr_cmd), "AT+IPR=%d", fast_baud);
        if (at_cmd_ok(ipr_cmd, AT_SHORT_TIMEOUT_MS)) {
            vTaskDelay(pdMS_TO_TICKS(100));
            
            uart_set_baudrate(UART_NUM, fast_baud);
            vTaskDelay(pdMS_TO_TICKS(100));
            
            if (at_cmd_ok("AT", AT_SHORT_TIMEOUT_MS)) {
                s_cell.config.baud_rate = fast_baud;
                ESP_LOGI(TAG, "UART baud rate upgraded: 115200 -> %d", fast_baud);
            } else {
                
                ESP_LOGW(TAG, "Fast baud failed, reverting to 115200");
                uart_set_baudrate(UART_NUM, 115200);
                vTaskDelay(pdMS_TO_TICKS(100));
                
                at_cmd_ok("AT+IPR=115200", AT_SHORT_TIMEOUT_MS);
            }
        }
    }

    
    err = sim_setup();
    if (err != ESP_OK) return err;

    
    err = network_register();
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "Cellular modem ready: %s IMEI=%s Operator=%s RSSI=%d",
             s_cell.info.model, s_cell.info.imei,
             s_cell.info.operator_name, s_cell.info.rssi);

    return ESP_OK;
}

void ml_cellular_deinit(void)
{
    if (s_cell.state == ML_CELL_STATE_OFF) return;

    
    if (s_cell.ppp_running) {
        ml_cellular_ppp_stop();
    }

    
    if (s_cell.uart_installed) {
        uart_driver_delete(UART_NUM);
        s_cell.uart_installed = false;
    }

    memset(&s_cell.info, 0, sizeof(s_cell.info));
    s_cell.state = ML_CELL_STATE_OFF;
    ESP_LOGI(TAG, "Cellular deinitialized");
}

esp_err_t ml_cellular_ppp_start(void)
{
    if (s_cell.state != ML_CELL_STATE_REGISTERED) {
        ESP_LOGE(TAG, "Cannot start PPP: not registered (state=%d)", s_cell.state);
        return ESP_ERR_INVALID_STATE;
    }

    return ppp_setup_and_dial();
}

esp_err_t ml_cellular_ppp_stop(void)
{
    if (!s_cell.ppp_running && !s_cell.ppp_netif) return ESP_OK;

    ESP_LOGI(TAG, "Stopping PPP...");

    
    s_cell.ppp_running = false;
    vTaskDelay(pdMS_TO_TICKS(500)); 

    





    if (s_cell.ppp_netif) {
        
        if (s_cell.ppp_events) {
            xEventGroupClearBits(s_cell.ppp_events, PPP_CLOSED_BIT);
        }

        
        esp_netif_action_disconnected(s_cell.ppp_netif, 0, 0, 0);
        esp_netif_action_stop(s_cell.ppp_netif, 0, 0, 0);

        


        if (s_cell.ppp_events) {
            ESP_LOGI(TAG, "Waiting for PPP close callback...");
            EventBits_t bits = xEventGroupWaitBits(
                s_cell.ppp_events, PPP_CLOSED_BIT,
                pdFALSE, pdFALSE,
                pdMS_TO_TICKS(15000));  
            if (bits & PPP_CLOSED_BIT) {
                ESP_LOGI(TAG, "PPP close confirmed");
            } else {
                ESP_LOGW(TAG, "PPP close timeout — proceeding with teardown");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100)); 
    }

    
    if (s_cell.ppp_status_handler) {
        esp_event_handler_instance_unregister(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, s_cell.ppp_status_handler);
        s_cell.ppp_status_handler = NULL;
    }
    if (s_cell.ip_event_handler) {
        esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, s_cell.ip_event_handler);
        s_cell.ip_event_handler = NULL;
    }

    
    if (s_cell.ppp_netif) {
        esp_netif_destroy(s_cell.ppp_netif);
        s_cell.ppp_netif = NULL;
    }

    if (s_cell.ppp_events) {
        vEventGroupDelete(s_cell.ppp_events);
        s_cell.ppp_events = NULL;
    }

    
    vTaskDelay(pdMS_TO_TICKS(1100)); 
    uart_write_bytes(UART_NUM, "+++", 3);
    vTaskDelay(pdMS_TO_TICKS(1100)); 

    char resp[256];
    at_send_cmd("AT", resp, sizeof(resp), AT_SHORT_TIMEOUT_MS);

    
    at_cmd_ok("ATH", AT_TIMEOUT_MS);

    s_cell.info.data_connected = false;
    if (s_cell.info.registered) {
        s_cell.state = ML_CELL_STATE_REGISTERED;
    } else {
        s_cell.state = ML_CELL_STATE_AT_OK;
    }

    ESP_LOGI(TAG, "PPP stopped, returned to AT mode");
    return ESP_OK;
}

ml_cellular_state_t ml_cellular_get_state(void)
{
    return s_cell.state;
}

esp_err_t ml_cellular_get_info(ml_cellular_info_t *info)
{
    if (!info) return ESP_ERR_INVALID_ARG;
    *info = s_cell.info;
    return ESP_OK;
}

const char *ml_cellular_get_imei(void)
{
    return s_cell.info.imei[0] ? s_cell.info.imei : NULL;
}

int ml_cellular_send_at(const char *cmd, char *response, size_t resp_size, int timeout_ms)
{
    if (s_cell.ppp_running) {
        ESP_LOGW(TAG, "Cannot send AT commands in PPP mode");
        return -1;
    }
    if (s_cell.state == ML_CELL_STATE_DATA_CONNECTED) {
        ESP_LOGW(TAG, "Cannot send raw AT in AT socket bridge mode — use ml_at_socket API");
        return -1;
    }
    return at_send_cmd(cmd, response, resp_size, timeout_ms);
}





esp_err_t ml_cellular_data_start(void)
{
    if (s_cell.state < ML_CELL_STATE_REGISTERED) {
        ESP_LOGE(TAG, "Cannot start data mode — not registered on network (state=%d)", s_cell.state);
        return ESP_ERR_INVALID_STATE;
    }

    if (s_cell.ppp_running) {
        ESP_LOGW(TAG, "PPP is running — stop it first");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_cell.state == ML_CELL_STATE_DATA_CONNECTED) {
        ESP_LOGW(TAG, "AT socket bridge already active");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting AT socket bridge data mode...");

    
    esp_err_t ret = ml_at_socket_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AT socket bridge init failed: %s", esp_err_to_name(ret));
        s_cell.state = ML_CELL_STATE_ERROR;
        return ret;
    }

    s_cell.state = ML_CELL_STATE_DATA_CONNECTED;
    s_cell.info.data_connected = true;
    ESP_LOGI(TAG, "AT socket bridge data mode ACTIVE");
    return ESP_OK;
}

esp_err_t ml_cellular_data_stop(void)
{
    if (s_cell.state != ML_CELL_STATE_DATA_CONNECTED) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping AT socket bridge data mode...");

    ml_at_socket_deinit();

    s_cell.info.data_connected = false;
    if (s_cell.info.registered) {
        s_cell.state = ML_CELL_STATE_REGISTERED;
    } else {
        s_cell.state = ML_CELL_STATE_AT_OK;
    }

    ESP_LOGI(TAG, "AT socket bridge stopped");
    return ESP_OK;
}

bool ml_cellular_is_data_mode(void)
{
    return s_cell.state == ML_CELL_STATE_DATA_CONNECTED;
}

esp_err_t ml_cellular_connect(void)
{
    if (s_cell.state < ML_CELL_STATE_REGISTERED) {
        ESP_LOGE(TAG, "Cannot connect data: not registered on network (state=%d)",
                 s_cell.state);
        return ESP_ERR_INVALID_STATE;
    }

    
    ESP_LOGI(TAG, "=== Attempting PPP data connection ===");
    esp_err_t ret = ml_cellular_ppp_start();

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "PPP connected — using lwIP standard sockets");
        s_cell.data_mode = ML_DATA_MODE_PPP;
        return ESP_OK;
    }

    
    ESP_LOGW(TAG, "PPP failed (%s) — falling back to AT socket bridge",
             esp_err_to_name(ret));
    ml_cellular_ppp_stop();

    
    vTaskDelay(pdMS_TO_TICKS(2000));

    
    ESP_LOGI(TAG, "=== Starting AT socket bridge (fallback) ===");
    ret = ml_cellular_data_start();

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "AT socket bridge connected — using modem internal TCP/IP");
        s_cell.data_mode = ML_DATA_MODE_AT_SOCKET;
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Both PPP and AT socket bridge failed");
    s_cell.data_mode = ML_DATA_MODE_NONE;
    return ret;
}

ml_cellular_data_mode_t ml_cellular_get_data_mode(void)
{
    return s_cell.data_mode;
}

#endif 
