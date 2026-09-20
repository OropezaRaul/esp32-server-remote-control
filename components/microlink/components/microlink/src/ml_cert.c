/*
 * ml_cert.c — NVS storage para certs TLS del ESP32
 *
 * El ESP32 recibe certs vía HTTP POST /api/cert/set desde cocopa01 (que ejecuta
 * acme.sh + dns_tailscale). El cert se guarda en NVS namespace
 * `nvs_tailscale_cert` con dos blobs:
 *   - `tls_cert`: fullchain PEM (cert + intermediate)
 *   - `tls_key`:  ECDSA private key PEM
 *
 * El TLS server (web_server_funnel_tls_start) intenta cargar de NVS primero;
 * si falla, usa el cert autofirmado hardcoded como fallback.
 *
 * Bytes totales esperados:
 *   - cert:  ~1.5-2 KB (Let's Encrypt ECDSA + chain)
 *   - key:   ~0.5 KB (ECDSA P-256)
 *   - NVS entry overhead: ~32 bytes por blob
 */

#include "ml_cert.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <stdlib.h>
#include "mbedtls/base64.h"

static const char *TAG = "ml_cert";

#define NVS_NAMESPACE "tailscale_cert"
#define NVS_KEY_CERT  "tls_cert"
#define NVS_KEY_CERT2 "tls_cert2"
#define NVS_KEY_KEY   "tls_key"

/*
 * NVS blobs viven en una sola página flash: el máximo real de nvs_set_blob es
 * ~4060 bytes. Un fullchain de Let's Encrypt (leaf + intermediate) ronda los
 * 5KB, así que lo partimos: `tls_cert` = primer bloque PEM (leaf),
 * `tls_cert2` = resto de la chain (intermediate). ml_cert_load() concatena
 * ambos para que el caller vea siempre un único fullchain.
 */
#define ML_CERT_PART_MAX 4000
#define ML_CERT_KEY_MAX_LEN 2048

/* Parte el fullchain en leaf (primer bloque PEM) y chain (resto).
 * Retorna las longitudes; chain_len=0 si hay un solo bloque. */
static esp_err_t split_fullchain(const uint8_t *pem, size_t len,
                                 size_t *leaf_len, size_t *chain_off) {
    const char *p = (const char *)pem;
    const char *end_marker = NULL;
    const char *q = p;
    /* Primer bloque: buscar el primer "-----END CERTIFICATE-----" */
    for (const char *s = q; (s = strstr(s, "-----END CERTIFICATE-----")) != NULL; s += 1) {
        end_marker = s;
        break;
    }
    if (!end_marker) return ESP_ERR_INVALID_ARG;
    /* fin del bloque: la línea después del marker */
    const char *nl = strchr(end_marker, '\n');
    size_t first = nl ? (size_t)(nl + 1 - p) : (size_t)(end_marker + 24 - p);
    if (first == 0 || first >= len) return ESP_ERR_INVALID_ARG;
    *leaf_len = first;
    *chain_off = first;
    return ESP_OK;
}

/* === Store ===
 * Persiste cert+key en NVS. Si ya existe uno, lo sobreescribe.
 * El fullchain se parte en leaf + chain si supera el máximo de un blob.
 * Retorna ESP_OK si commit() tuvo éxito. */
esp_err_t ml_cert_store(const uint8_t *cert_pem, size_t cert_len,
                        const uint8_t *key_pem,  size_t key_len) {
    if (cert_pem == NULL || key_pem == NULL || cert_len == 0 || key_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (key_len > ML_CERT_KEY_MAX_LEN) {
        ESP_LOGE(TAG, "key too big: %u (max %u)",
                 (unsigned)key_len, ML_CERT_KEY_MAX_LEN);
        return ESP_ERR_INVALID_SIZE;
    }

    size_t leaf_len = cert_len, chain_off = cert_len;
    esp_err_t sc = split_fullchain(cert_pem, cert_len, &leaf_len, &chain_off);
    if (sc != ESP_OK) {
        /* Un solo bloque o PEM no reconocido: guardar tal cual (límite blob) */
        if (cert_len > ML_CERT_PART_MAX) {
            ESP_LOGE(TAG, "cert too big: %u (max %u)", (unsigned)cert_len, ML_CERT_PART_MAX);
            return ESP_ERR_INVALID_SIZE;
        }
        leaf_len = cert_len;
        chain_off = cert_len;
    } else {
        size_t chain_len = cert_len - chain_off;
        if (leaf_len > ML_CERT_PART_MAX || chain_len > ML_CERT_PART_MAX) {
            ESP_LOGE(TAG, "cert part too big: leaf=%u chain=%u (max %u)",
                     (unsigned)leaf_len, (unsigned)chain_len, ML_CERT_PART_MAX);
            return ESP_ERR_INVALID_SIZE;
        }
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(nvs, NVS_KEY_CERT, cert_pem, leaf_len);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, NVS_KEY_KEY, key_pem, key_len);
    }
    if (err == ESP_OK) {
        size_t chain_len = cert_len - chain_off;
        if (chain_len > 0) {
            err = nvs_set_blob(nvs, NVS_KEY_CERT2, cert_pem + chain_off, chain_len);
        } else {
            nvs_erase_key(nvs, NVS_KEY_CERT2);  /* opcional; sin chain */
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_blob failed: %s", esp_err_to_name(err));
        nvs_close(nvs);
        return err;
    }
    err = nvs_commit(nvs);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "cert stored: leaf=%u chain=%u key=%u",
                 (unsigned)leaf_len, (unsigned)(cert_len - chain_off),
                 (unsigned)key_len);
    } else {
        ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(err));
    }
    nvs_close(nvs);
    return err;
}

/* === Load ===
 * Reserva y devuelve cert+key. El caller debe free() ambos punteros.
 * Concatena leaf + chain (tls_cert + tls_cert2) en un único buffer.
 * Retorna ESP_ERR_NOT_FOUND si NVS está vacío (fallback a hardcoded). */
esp_err_t ml_cert_load(uint8_t **cert_pem, size_t *cert_len,
                       uint8_t **key_pem,  size_t *key_len) {
    if (cert_pem == NULL || key_pem == NULL || cert_len == NULL || key_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;  /* NVS namespace no existe todavía */
    }

    size_t cl = 0, cl2 = 0, kl = 0;
    err = nvs_get_blob(nvs, NVS_KEY_CERT, NULL, &cl);
    if (err != ESP_OK || cl == 0) {
        nvs_close(nvs);
        return ESP_ERR_NOT_FOUND;
    }
    /* chain opcional (blobs viejos no la tienen) */
    esp_err_t e2 = nvs_get_blob(nvs, NVS_KEY_CERT2, NULL, &cl2);
    if (e2 != ESP_OK) cl2 = 0;
    err = nvs_get_blob(nvs, NVS_KEY_KEY, NULL, &kl);
    if (err != ESP_OK || kl == 0) {
        nvs_close(nvs);
        return ESP_ERR_NOT_FOUND;
    }

    /* +1 para null-terminator en ambos (mbedtls_x509_crt_parse y
     * mbedtls_pk_parse_key SOLO detectan PEM si buf[len-1] == '\0'). */
    uint8_t *c = malloc(cl + cl2 + 1);
    uint8_t *k = malloc(kl + 1);
    if (c == NULL || k == NULL) {
        free(c); free(k);
        nvs_close(nvs);
        return ESP_ERR_NO_MEM;
    }

    err = nvs_get_blob(nvs, NVS_KEY_CERT, c, &cl);
    if (err == ESP_OK && cl2 > 0) {
        err = nvs_get_blob(nvs, NVS_KEY_CERT2, c + cl, &cl2);
        if (err == ESP_OK) cl += cl2;
    }
    if (err == ESP_OK) err = nvs_get_blob(nvs, NVS_KEY_KEY, k, &kl);
    nvs_close(nvs);
    if (err != ESP_OK) { free(c); free(k); return err; }

    c[cl] = '\0';
    *cert_pem = c;
    *cert_len = cl;
    *key_pem  = k;
    *key_len  = kl;
    k[kl] = '\0';

    nvs_close(nvs);
    ESP_LOGI(TAG, "cert loaded: %u bytes cert, %u bytes key",
             (unsigned)cl, (unsigned)kl);
    return ESP_OK;
}

/* === Clear ===
 * Borra ambos blobs. Útil para rollback o testing. */
esp_err_t ml_cert_clear(void) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    err = nvs_erase_all(nvs);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "cert cleared");
    }
    return err;
}

/* === Info ===
 * Reporta si hay cert en NVS y su tamaño. Útil para /diag. */
esp_err_t ml_cert_info(bool *present, size_t *cert_len, size_t *key_len) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        *present = false;
        if (cert_len) *cert_len = 0;
        if (key_len)  *key_len  = 0;
        return ESP_OK;  /* No es error, simplemente no hay cert */
    }

    size_t cl = 0, kl = 0;
    esp_err_t e1 = nvs_get_blob(nvs, NVS_KEY_CERT, NULL, &cl);
    esp_err_t e2 = nvs_get_blob(nvs, NVS_KEY_KEY,  NULL, &kl);
    nvs_close(nvs);

    *present = (e1 == ESP_OK && cl > 0 && e2 == ESP_OK && kl > 0);
    if (cert_len) *cert_len = cl;
    if (key_len)  *key_len  = kl;
    return ESP_OK;
}

/* === PEM → DER ===
 * Convierte un string PEM (BEGIN ... END) a bytes DER binarios.
 * Elimina headers/footers/whitespace y base64-decodifica.
 *
 * Útil porque mbedTLS en ESP32-S3 v3.x tiene problemas parseando algunos PEM
 * (especialmente con newlines incrustados), pero siempre parsea DER bien.
 *
 * @param pem      String PEM con BEGIN/END markers.
 * @param pem_len  Longitud del string (puede ser > longitud real).
 * @param[out] der Malloc'd buffer con bytes DER. Caller free().
 * @param[out] der_len Longitud de los bytes DER.
 * @return ESP_OK si OK.
 */
esp_err_t ml_pem_to_der(const char *pem, size_t pem_len,
                        uint8_t **der, size_t *der_len) {
    if (pem == NULL || der == NULL || der_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Encontrar el bloque "-----BEGIN ... -----" */
    const char *begin = strstr(pem, "-----BEGIN");
    if (begin == NULL) {
        return ESP_ERR_INVALID_ARG;  /* No es PEM */
    }
    const char *body_start = strstr(begin, "-----BEGIN");
    if (body_start == NULL) return ESP_ERR_INVALID_ARG;

    /* Avanzar hasta el primer \n después del header */
    const char *line_start = strchr(body_start, '\n');
    if (line_start == NULL) return ESP_ERR_INVALID_ARG;
    line_start++;  /* saltar el \n */

    /* Encontrar "-----END ... -----" */
    const char *end_marker = strstr(line_start, "-----END");
    if (end_marker == NULL) return ESP_ERR_INVALID_ARG;

    /* El body está entre line_start y end_marker */
    size_t body_len = end_marker - line_start;
    if (body_len == 0 || body_len > 8192) {
        return ESP_ERR_INVALID_SIZE;
    }

    /* Extraer body en buffer temporal, ignorando whitespace */
    uint8_t *body = malloc(body_len);
    if (body == NULL) return ESP_ERR_NO_MEM;

    size_t body_idx = 0;
    for (size_t i = 0; i < body_len; i++) {
        char c = line_start[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        body[body_idx++] = (uint8_t)c;
    }

    /* Base64 decode */
    size_t out_len = 0;
    int ret = mbedtls_base64_decode(NULL, 0, &out_len, body, body_idx);
    if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        free(body);
        ESP_LOGE(TAG, "ml_pem_to_der: base64 size probe failed: -0x%04X", -ret);
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t *out = malloc(out_len);
    if (out == NULL) {
        free(body);
        return ESP_ERR_NO_MEM;
    }

    ret = mbedtls_base64_decode(out, out_len, &out_len, body, body_idx);
    free(body);
    if (ret != 0) {
        free(out);
        ESP_LOGE(TAG, "ml_pem_to_der: base64 decode failed: -0x%04X", -ret);
        return ESP_ERR_INVALID_ARG;
    }

    *der = out;
    *der_len = out_len;
    return ESP_OK;
}