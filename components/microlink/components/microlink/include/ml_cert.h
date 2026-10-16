







#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif













esp_err_t ml_cert_store(const uint8_t *cert_pem, size_t cert_len,
                        const uint8_t *key_pem,  size_t key_len);












esp_err_t ml_cert_load(uint8_t **cert_pem, size_t *cert_len,
                       uint8_t **key_pem,  size_t *key_len);




esp_err_t ml_cert_clear(void);








esp_err_t ml_cert_info(bool *present, size_t *cert_len, size_t *key_len);











esp_err_t ml_pem_to_der(const char *pem, size_t pem_len,
                        uint8_t **der, size_t *der_len);

#ifdef __cplusplus
}
#endif