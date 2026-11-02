





















#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif












esp_err_t ml_peerapi_start(uint16_t port_v4, uint16_t port_v6);




esp_err_t ml_peerapi_stop(void);




uint16_t ml_peerapi_get_port(void);




uint16_t ml_peerapi_get_port6(void);






void ml_peerapi_set_vpn_ip(uint32_t ip);






uint32_t ml_peerapi_get_enfile_hits(void);

#ifdef __cplusplus
}
#endif
