/*
 * ml_cert.h — API pública para storage de certs TLS
 *
 * El ESP32 guarda certs emitidos por Let's Encrypt (via cliente ACME interno
 * o script externo) en NVS namespace `nvs_tailscale_cert`. El TLS server carga
 * de NVS primero, fallback al cert hardcoded.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Persiste cert+key en NVS. Si ya existe uno, lo sobreescribe.
 *
 * @param cert_pem  Fullchain PEM (cert + intermediate). No requiere null-terminator.
 * @param cert_len  Longitud en bytes.
 * @param key_pem   Private key PEM.
 * @param key_len   Longitud en bytes.
 * @return ESP_OK si commit tuvo éxito.
 *         ESP_ERR_INVALID_ARG si params nulos o vacíos.
 *         ESP_ERR_INVALID_SIZE si excede los límites (4KB cert / 2KB key).
 *         ESP_ERR_NO_MEM si malloc falla (no debería pasar, usa NVS directo).
 */
esp_err_t ml_cert_store(const uint8_t *cert_pem, size_t cert_len,
                        const uint8_t *key_pem,  size_t key_len);

/**
 * Reserva y devuelve cert+key de NVS. Caller debe free() ambos punteros.
 *
 * @param[out] cert_pem  Malloc'd buffer con cert PEM. Caller free().
 * @param[out] cert_len  Longitud del cert.
 * @param[out] key_pem   Malloc'd buffer con key PEM. Caller free().
 * @param[out] key_len   Longitud de la key.
 * @return ESP_OK si cargó OK.
 *         ESP_ERR_NOT_FOUND si NVS está vacío (usar fallback).
 *         ESP_ERR_NO_MEM si malloc falla.
 */
esp_err_t ml_cert_load(uint8_t **cert_pem, size_t *cert_len,
                       uint8_t **key_pem,  size_t *key_len);

/**
 * Borra el cert de NVS. Útil para rollback.
 */
esp_err_t ml_cert_clear(void);

/**
 * Reporta si hay cert en NVS. Más barato que ml_cert_load() porque no copia.
 *
 * @param[out] present    true si hay cert completo.
 * @param[out] cert_len   Tamaño del cert (0 si no hay).
 * @param[out] key_len    Tamaño de la key (0 si no hay).
 */
esp_err_t ml_cert_info(bool *present, size_t *cert_len, size_t *key_len);

/**
 * Convierte un string PEM a bytes DER. Útil porque mbedTLS parsea DER
 * consistentemente pero falla con algunos PEM (newlines incrustados).
 *
 * @param pem       String PEM (puede tener whitespace extra).
 * @param pem_len   Longitud del string.
 * @param[out] der  Malloc'd buffer DER. Caller debe free().
 * @param[out] der_len  Longitud en bytes.
 * @return ESP_OK si OK.
 */
esp_err_t ml_pem_to_der(const char *pem, size_t pem_len,
                        uint8_t **der, size_t *der_len);

#ifdef __cplusplus
}
#endif