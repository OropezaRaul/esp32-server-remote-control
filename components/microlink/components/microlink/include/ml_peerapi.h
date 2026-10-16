





















#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif









esp_err_t ml_peerapi_start(uint16_t port);




esp_err_t ml_peerapi_stop(void);




uint16_t ml_peerapi_get_port(void);

#ifdef __cplusplus
}
#endif
