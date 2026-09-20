#pragma once

#include <esp_err.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

ESP_EVENT_DECLARE_BASE(WIFI_EVENT_BASE);

extern EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

esp_err_t wifi_init_sta(void);
bool wifi_is_connected(void);
esp_ip4_addr_t wifi_get_ip(void);