/*
 * ml_acme.h — Cliente ACME (RFC 8555) mínimo contra Let's Encrypt.
 *
 * Obtiene un cert TLS para un hostname vía challenge DNS-01 usando las claves
 * EC P-256 persistidas en NVS (account key para JWS, leaf key para el CSR).
 * El fullchain PEM y la leaf key PEM se persisten vía ml_cert_store().
 *
 * Uso típico:
 *   ml_acme_set_dns_publisher(publisher_fn);   // implementado por otro módulo
 *   ml_acme_set_progress_cb(progress_cb);      // opcional
 *   ml_acme_set_directory(MY_ACME_DIR);        // opcional (override staging)
 *   ml_acme_init();                            // carga/genera claves en NVS
 *   ml_acme_obtain_cert("host.tailnet.ts.net");
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Publica un TXT record para el challenge DNS-01. Implementado por otro
 * módulo (puente al control plane de Tailscale). Bloquea hasta que el
 * publisher confirme o timeout. name incluye el full record name
 * (ej: "_acme-challenge.esp32-node.your-tailnet.ts.net"), value es el
 * digest base64url del key authorization. */
typedef esp_err_t (*ml_acme_set_dns_fn)(const char *name, const char *value,
                                        uint32_t timeout_ms);

typedef void (*ml_acme_progress_fn)(const char *stage, esp_err_t err);

/* Override del ACME directory URL. Por defecto staging de Let's Encrypt. */
esp_err_t ml_acme_set_directory(const char *url);

esp_err_t ml_acme_set_dns_publisher(ml_acme_set_dns_fn fn);
esp_err_t ml_acme_set_progress_cb(ml_acme_progress_fn cb);

/* Carga (o genera la primera vez) las claves de NVS:
 * namespace "ml_acme": "acct_key" (blob 32 bytes, EC P-256 private scalar,
 * big-endian) y "leaf_key" (idem). Llamar una vez antes de obtain_cert. */
esp_err_t ml_acme_init(void);

/* Flujo ACME completo para hostname (ej "esp32-node.your-tailnet.ts.net"):
 * directory → newAccount → newOrder → authz dns-01 → set_dns publisher →
 * challenge → poll authz → finalize(CSR) → poll → download cert →
 * ml_cert_store(fullchain_pem, leaf_key_pem).
 * En éxito devuelve ESP_OK. En fallo, ESP_FAIL y ml_acme_last_error()
 * describe la etapa/motivo. No reintentar internamente más de 2 veces por
 * request HTTP. */
esp_err_t ml_acme_obtain_cert(const char *hostname);

const char *ml_acme_last_error(void);

/* Unix timestamp de la última emisión exitosa (NVS "ml_acme"/"last_issue");
 * 0 si nunca. Para lógica de renovación externa. */
int64_t ml_acme_last_issue_unix(void);

#ifdef __cplusplus
}
#endif
