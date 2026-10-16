#pragma once

#include <esp_err.h>
#include <stdint.h>
#include <stdbool.h>

esp_err_t wol_parse_mac(const char *mac_str, uint8_t out_mac[6]);
esp_err_t wol_send(const uint8_t mac[6]);
esp_err_t wol_send_str(const char *mac_str);
esp_err_t wol_send_default(void);

// Instala (o re-confirma) la entrada ARP estatica 192.168.1.100 -> MAC del
// servidor. Idempotente. Se llama desde wake_init() para que el unicast del
// WOL y las sondas TCP salen con la MAC correcta desde el boot, sin depender
// de que alguien haya pedido un WOL antes.
esp_err_t wol_prime_arp(void);
