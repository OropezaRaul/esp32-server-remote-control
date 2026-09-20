#pragma once

#include <esp_err.h>
#include <stdint.h>
#include <stdbool.h>

esp_err_t wol_parse_mac(const char *mac_str, uint8_t out_mac[6]);
esp_err_t wol_send(const uint8_t mac[6]);
esp_err_t wol_send_str(const char *mac_str);
esp_err_t wol_send_default(void);