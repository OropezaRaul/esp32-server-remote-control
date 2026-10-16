#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>

#include "microlink.h"

esp_err_t microlink_wrapper_start(void);
esp_err_t microlink_wrapper_stop(void);
bool microlink_wrapper_is_connected(void);
uint32_t microlink_wrapper_get_vpn_ip(void);



microlink_state_t microlink_wrapper_get_state(void);




uint32_t microlink_wrapper_get_last_connected_ms(void);








uint32_t microlink_wrapper_get_ctrl_stale_ms(void);


const char *microlink_wrapper_state_name(microlink_state_t s);







esp_err_t microlink_wrapper_rebind(void);




esp_err_t microlink_wrapper_trigger_handshake(uint32_t dest_vpn_ip);




esp_err_t microlink_wrapper_send_cmm(uint32_t dest_vpn_ip);


bool microlink_wrapper_peer_is_up(uint32_t dest_vpn_ip);


const char *microlink_wrapper_get_fqdn(void);



esp_err_t microlink_wrapper_set_dns(const char *name, const char *value,
                                    uint32_t timeout_ms);
