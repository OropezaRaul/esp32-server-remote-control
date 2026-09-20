/*
 * ml_acme.c — Cliente ACME v2 (RFC 8555) mínimo contra Let's Encrypt.
 *
 * Implementa el flujo completo DNS-01 para un único hostname:
 *   directory → newAccount → newOrder → authz → challenge → poll authz →
 *   finalize (CSR) → poll order → download cert → ml_cert_store().
 *
 * Criptografía:
 *   - JWS (RFC 7515) sobre ES256. La account key es EC P-256 persistida en NVS
 *     como blob de 32 bytes (scalar d big-endian). Tras newAccount se guarda
 *     la account URL (kid) y se usa en vez de jwk en requests posteriores.
 *   - CSR generado con mbedtls_x509write_csr_der; SAN (dNSName) construido a
 *     mano en DER y agregado vía mbedtls_x509write_csr_set_extension.
 *
 * HTTP:
 *   esp_http_client nuevo por request. Cert bundle default del IDF para validar
 *   TLS contra Let's Encrypt. Capturamos Replay-Nonce y Location con un
 *   HTTP_EVENT_ON_HEADER handler estático (esp_http_client_get_headers no
 *   existe en IDF 5.x; usamos el singular esp_http_client_get_header cuando
 *   se necesita y un event handler para el nonce).
 *
 * No se debe llamar ml_acme_obtain_cert más de una vez en paralelo; las
 * claves y buffers estáticos no son re-entrantes.
 */

#ifndef MBEDTLS_ALLOW_PRIVATE_ACCESS
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#endif

#include "sdkconfig.h"
#include "ml_acme.h"
#include "ml_cert.h"

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <inttypes.h>
#include <time.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#include "nvs_flash.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"

#include "mbedtls/ecp.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "mbedtls/base64.h"
#include "mbedtls/x509_csr.h"
#include "mbedtls/oid.h"
#include "mbedtls/asn1write.h"
#include "mbedtls/error.h"
#include "mbedtls/md.h"

#include "esp_random.h"

static const char *TAG = "ml_acme";

#define ML_ACME_DIRECTORY_DEFAULT "https://acme-staging-v02.api.letsencrypt.org/directory"
#define ML_ACME_NVS_NAMESPACE    "ml_acme"
#define ML_ACME_NVS_ACCT_KEY     "acct_key"
#define ML_ACME_NVS_LEAF_KEY     "leaf_key"
#define ML_ACME_NVS_LAST_ISSUE   "last_issue"
#define ML_ACME_NVS_KID          "kid"
#define ML_ACME_NVS_DIR_OVERRIDE "dir_url"

#define ML_ACME_RESP_CAP         (32 * 1024)   /* límite duro del body */
#define ML_ACME_RESP_INIT        (4 * 1024)
#define ML_ACME_HTTP_TIMEOUT_MS  15000
#define ML_ACME_PROPAGATION_MS   15000
#define ML_ACME_POLL_ORDER_MS    5000
#define ML_ACME_POLL_ORDER_MAX_MS (60 * 1000)
#define ML_ACME_POLL_AUTHZ_MS    5000
#define ML_ACME_POLL_AUTHZ_MAX_MS (120 * 1000)
#define ML_ACME_PUBLISH_TIMEOUT_MS (120 * 1000)
#define ML_ACME_HTTP_RETRY_MAX   2             /* 2 reintentos por request */
#define ML_ACME_KEY_BYTES        32            /* EC P-256 scalar size */

/* ───────────────────────── Estado estático ─────────────────────────── */

static char             s_directory_url[256] = ML_ACME_DIRECTORY_DEFAULT;
static ml_acme_set_dns_fn   s_set_dns = NULL;
static ml_acme_progress_fn  s_progress = NULL;

static mbedtls_pk_context s_acct_pk;
static mbedtls_pk_context s_leaf_pk;
static bool              s_keys_loaded = false;

static char             s_kid[256] = {0};      /* account URL (kid) */
static char             s_last_nonce[128] = {0};
static char             s_last_error[128] = {0};

/* Buffers persistentes para el nonce (no se libera entre requests): son
 * propiedad del módulo y se rellenan vía HTTP_EVENT_ON_HEADER. */

/* ───────────────────────── Forward declarations ────────────────────── */

static int ml_acme_rng(void *ctx, unsigned char *buf, size_t len);
static void progress(const char *stage, esp_err_t err);
static void set_error(const char *fmt, ...);

/* ───────────────────────── Utilidades varias ───────────────────────── */

/* Base64 URL-safe sin padding (RFC 7515). */
static size_t b64url_encode(const unsigned char *src, size_t src_len,
                            char *dst, size_t dst_cap)
{
    size_t olen = 0;
    if (mbedtls_base64_encode((unsigned char *)dst, dst_cap, &olen, src, src_len) != 0) {
        return 0;
    }
    /* Reemplazar +/= por -_ y stripping '=' final. */
    for (size_t i = 0; i < olen; i++) {
        if (dst[i] == '+') dst[i] = '-';
        else if (dst[i] == '/') dst[i] = '_';
    }
    while (olen > 0 && dst[olen - 1] == '=') {
        dst[--olen] = '\0';
    }
    return olen;
}

/* SHA-256 → base64url. Devuelve longitud escrita. */
static int sha256_b64url(const unsigned char *in, size_t in_len, char *out, size_t out_cap)
{
    unsigned char digest[32];
    if (mbedtls_sha256(in, in_len, digest, 0) != 0) {
        return -1;
    }
    size_t n = b64url_encode(digest, sizeof(digest), out, out_cap);
    return (n > 0) ? (int)n : -1;
}

/* strdup con malloc. */
static char *xstrdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

/* ───────────────────────── Claves EC P-256 (NVS) ───────────────────── */

static esp_err_t load_or_gen_key(const char *blob_key, mbedtls_pk_context *pk)
{
    esp_err_t err;
    nvs_handle_t nvs;
    uint8_t scalar[ML_ACME_KEY_BYTES];
    size_t len = sizeof(scalar);
    bool have = false;

    err = nvs_open(ML_ACME_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        set_error("nvs_open %s: %s", blob_key, esp_err_to_name(err));
        return err;
    }
    err = nvs_get_blob(nvs, blob_key, scalar, &len);
    if (err == ESP_OK && len == ML_ACME_KEY_BYTES) {
        have = true;
    }

    mbedtls_pk_init(pk);
    int rc = mbedtls_pk_setup(pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (rc != 0) {
        set_error("pk_setup %s: -0x%x", blob_key, (unsigned)-rc);
        nvs_close(nvs);
        return ESP_FAIL;
    }

    if (!have) {
        rc = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,
                                 mbedtls_pk_ec(*pk),
                                 ml_acme_rng, NULL);
        if (rc != 0) {
            set_error("ecp_gen_key %s: -0x%x", blob_key, (unsigned)-rc);
            mbedtls_pk_free(pk);
            nvs_close(nvs);
            return ESP_FAIL;
        }
        size_t wlen = 0;
        rc = mbedtls_ecp_write_key_ext(mbedtls_pk_ec(*pk), &wlen,
                                       scalar, sizeof(scalar));
        if (rc != 0 || wlen != sizeof(scalar)) {
            set_error("ecp_write_key_ext %s: rc=%d wlen=%u",
                      blob_key, rc, (unsigned)wlen);
            mbedtls_pk_free(pk);
            nvs_close(nvs);
            return ESP_FAIL;
        }
        err = nvs_set_blob(nvs, blob_key, scalar, sizeof(scalar));
        if (err != ESP_OK) {
            set_error("nvs_set_blob %s: %s", blob_key, esp_err_to_name(err));
            mbedtls_pk_free(pk);
            nvs_close(nvs);
            return err;
        }
        err = nvs_commit(nvs);
        if (err != ESP_OK) {
            set_error("nvs_commit %s: %s", blob_key, esp_err_to_name(err));
            mbedtls_pk_free(pk);
            nvs_close(nvs);
            return err;
        }
        ESP_LOGI(TAG, "%s generated and persisted", blob_key);
    } else {
        rc = mbedtls_ecp_read_key(MBEDTLS_ECP_DP_SECP256R1,
                                  mbedtls_pk_ec(*pk),
                                  scalar, sizeof(scalar));
        if (rc != 0) {
            set_error("ecp_read_key %s: -0x%x", blob_key, (unsigned)-rc);
            mbedtls_pk_free(pk);
            nvs_close(nvs);
            return ESP_FAIL;
        }
        /* Recalcular la public point Q = d*G (read_key solo carga d). */
        mbedtls_ecp_keypair *kp = mbedtls_pk_ec(*pk);
        rc = mbedtls_ecp_mul(&kp->grp, &kp->Q, &kp->d, &kp->grp.G,
                             ml_acme_rng, NULL);
        if (rc != 0) {
            set_error("ecp_mul pubkey %s: -0x%x", blob_key, (unsigned)-rc);
            mbedtls_pk_free(pk);
            nvs_close(nvs);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "%s loaded from NVS", blob_key);
    }
    nvs_close(nvs);
    return ESP_OK;
}

/* ───────────────────────── JWK / Thumbprint ────────────────────────── */

/* Construye {"crv":"P-256","kty":"EC","x":<b64url>,"y":<b64url>} con campos
 * en orden lexicográfico exacto (RFC 7638 §3.2). Devuelve malloc'd string. */
static char *build_jwk_canonical_json(mbedtls_pk_context *pk)
{
    unsigned char pub_xy[65];   /* uncompressed: 0x04 || X(32) || Y(32) */
    size_t olen = 0;
    int rc = mbedtls_ecp_point_write_binary(&mbedtls_pk_ec(*pk)->grp,
                                            &mbedtls_pk_ec(*pk)->Q,
                                            MBEDTLS_ECP_PF_UNCOMPRESSED,
                                            &olen, pub_xy, sizeof(pub_xy));
    if (rc != 0 || olen != 65 || pub_xy[0] != 0x04) {
        return NULL;
    }
    char xb[64], yb[64];
    if (b64url_encode(pub_xy + 1, 32, xb, sizeof(xb)) == 0) return NULL;
    if (b64url_encode(pub_xy + 33, 32, yb, sizeof(yb)) == 0) return NULL;
    size_t need = strlen(xb) + strlen(yb) + 64;
    char *out = (char *)malloc(need);
    if (!out) return NULL;
    snprintf(out, need, "{\"crv\":\"P-256\",\"kty\":\"EC\",\"x\":\"%s\",\"y\":\"%s\"}", xb, yb);
    return out;
}

/* Devuelve malloc'd b64url string. */
static char *build_jwk_json(mbedtls_pk_context *pk)
{
    unsigned char pub_xy[65];
    size_t olen = 0;
    int rc = mbedtls_ecp_point_write_binary(&mbedtls_pk_ec(*pk)->grp,
                                            &mbedtls_pk_ec(*pk)->Q,
                                            MBEDTLS_ECP_PF_UNCOMPRESSED,
                                            &olen, pub_xy, sizeof(pub_xy));
    if (rc != 0 || olen != 65) return NULL;
    char xb[64], yb[64];
    if (b64url_encode(pub_xy + 1, 32, xb, sizeof(xb)) == 0) return NULL;
    if (b64url_encode(pub_xy + 33, 32, yb, sizeof(yb)) == 0) return NULL;
    size_t need = strlen(xb) + strlen(yb) + 64;
    char *out = (char *)malloc(need);
    if (!out) return NULL;
    snprintf(out, need, "{\"kty\":\"EC\",\"crv\":\"P-256\",\"x\":\"%s\",\"y\":\"%s\"}", xb, yb);
    return out;
}

/* ───────────────────────── DER ECDSA sig parser ──────────────────────
 * Convierte SEQUENCE { INTEGER r, INTEGER s } (DER ASN.1) en raw r||s
 * de 64 bytes (32 cada uno, zero-padded a la izquierda). */
static int ecdsa_der_to_raw(const unsigned char *der, size_t der_len,
                            unsigned char raw[64])
{
    if (der == NULL || der_len < 8 || der[0] != 0x30) return -1;
    const unsigned char *p = der + 2;   /* skip SEQUENCE tag + len */
    size_t rem = der_len - 2;

    /* INTEGER r */
    if (rem < 2 || p[0] != 0x02) return -1;
    p++; rem--;
    size_t r_len = p[0]; p++; rem--;
    if (rem < r_len || r_len == 0) return -1;
    /* strip leading zero (sign-byte) si está y el siguiente bit7=1 */
    const unsigned char *rp = p;
    size_t r_use = r_len;
    if (r_use > 1 && rp[0] == 0x00 && (rp[1] & 0x80)) {
        rp++; r_use--;
    }
    if (r_use > 32) return -1;
    memset(raw, 0, 32);
    memcpy(raw + 32 - r_use, rp, r_use);
    p += r_len; rem -= r_len;

    /* INTEGER s */
    if (rem < 2 || p[0] != 0x02) return -1;
    p++; rem--;
    size_t s_len = p[0]; p++; rem--;
    if (rem < s_len || s_len == 0) return -1;
    const unsigned char *sp = p;
    size_t s_use = s_len;
    if (s_use > 1 && sp[0] == 0x00 && (sp[1] & 0x80)) {
        sp++; s_use--;
    }
    if (s_use > 32) return -1;
    memset(raw + 32, 0, 32);
    memcpy(raw + 64 - s_use, sp, s_use);
    return 0;
}

/* ───────────────────────── HTTP event handler ─────────────────────────
 * Captura Replay-Nonce y Location en respuestas (en minúsculas según
 * observación práctica con Let's Encrypt; comparamos case-insensitive). */
typedef struct {
    char replay_nonce[128];
    char location[512];
    bool got_nonce;
    bool got_location;
} http_capture_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (!evt || !evt->user_data) return ESP_OK;
    http_capture_t *cap = (http_capture_t *)evt->user_data;

    if (evt->event_id == HTTP_EVENT_ON_HEADER) {
        if (evt->header_key && evt->header_value) {
            if (strcasecmp(evt->header_key, "Replay-Nonce") == 0) {
                strncpy(cap->replay_nonce, evt->header_value, sizeof(cap->replay_nonce) - 1);
                cap->replay_nonce[sizeof(cap->replay_nonce) - 1] = '\0';
                cap->got_nonce = true;
                ESP_LOGI(TAG, "captured Replay-Nonce len=%u val=%.16s",
                         (unsigned)strlen(evt->header_value), evt->header_value);
            } else if (strcasecmp(evt->header_key, "Location") == 0) {
                strncpy(cap->location, evt->header_value, sizeof(cap->location) - 1);
                cap->location[sizeof(cap->location) - 1] = '\0';
                cap->got_location = true;
            }
        }
    }
    return ESP_OK;
}

/* ───────────────────────── JWS body builder ────────────────────────── */

typedef struct {
    char *protected_b64;
    char *payload_b64;
    char *signature_b64;
    char *json;          /* serializado completo, malloc'd */
    size_t json_len;
} jws_t;

static void jws_free(jws_t *j)
{
    if (!j) return;
    free(j->protected_b64);
    free(j->payload_b64);
    free(j->signature_b64);
    free(j->json);
    memset(j, 0, sizeof(*j));
}

/* Carga el protected header con jwk (no kid). */
static char *build_protected_with_jwk(mbedtls_pk_context *pk, const char *nonce,
                                      const char *url)
{
    char *jwk = build_jwk_json(pk);
    if (!jwk) return NULL;
    /* {"alg":"ES256","jwk":<jwk>,"nonce":<n>,"url":<u>} SIN espacios. */
    size_t need = strlen(jwk) + strlen(nonce) + strlen(url) + 64;
    char *out = (char *)malloc(need);
    if (!out) { free(jwk); return NULL; }
    snprintf(out, need, "{\"alg\":\"ES256\",\"jwk\":%s,\"nonce\":\"%s\",\"url\":\"%s\"}",
             jwk, nonce, url);
    free(jwk);
    return out;
}

/* Carga el protected header con kid (post-newAccount). */
static char *build_protected_with_kid(const char *kid, const char *nonce,
                                      const char *url)
{
    size_t need = strlen(kid) + strlen(nonce) + strlen(url) + 64;
    char *out = (char *)malloc(need);
    if (!out) return NULL;
    snprintf(out, need, "{\"alg\":\"ES256\",\"kid\":\"%s\",\"nonce\":\"%s\",\"url\":\"%s\"}",
             kid, nonce, url);
    return out;
}

/* payload_b64 puede ser NULL para post-as-get (string vacío ""). */
static esp_err_t jws_build(jws_t *out, mbedtls_pk_context *pk,
                           const char *nonce, const char *url,
                           const char *payload_json)
{
    memset(out, 0, sizeof(*out));

    /* 1) protected header */
    char *protected_raw = (s_kid[0] != '\0')
        ? build_protected_with_kid(s_kid, nonce, url)
        : build_protected_with_jwk(pk, nonce, url);
    if (!protected_raw) {
        set_error("build protected header: OOM");
        return ESP_ERR_NO_MEM;
    }

    /* 2) payload b64url (vacío si payload_json es NULL) */
    const char *payload_for_b64 = payload_json ? payload_json : "";
    size_t pl_len = strlen(payload_for_b64);
    size_t pl_cap = ((pl_len + 2) / 3) * 4 + 4;
    out->payload_b64 = (char *)malloc(pl_cap);
    if (!out->payload_b64) { free(protected_raw); set_error("OOM payload_b64"); return ESP_ERR_NO_MEM; }
    if (pl_len > 0) {
        if (b64url_encode((const unsigned char *)payload_for_b64, pl_len,
                          out->payload_b64, pl_cap) == 0) {
            free(protected_raw); free(out->payload_b64); out->payload_b64 = NULL;
            set_error("b64url(payload) failed"); return ESP_FAIL;
        }
    } else {
        out->payload_b64[0] = '\0';
    }

    /* 3) protected b64url */
    size_t pr_len = strlen(protected_raw);
    size_t pr_cap = ((pr_len + 2) / 3) * 4 + 4;
    out->protected_b64 = (char *)malloc(pr_cap);
    if (!out->protected_b64) { free(protected_raw); set_error("OOM protected_b64"); return ESP_ERR_NO_MEM; }
    if (b64url_encode((const unsigned char *)protected_raw, pr_len,
                      out->protected_b64, pr_cap) == 0) {
        free(protected_raw); jws_free(out);
        set_error("b64url(protected) failed"); return ESP_FAIL;
    }
    free(protected_raw);

    /* 4) signing input = "<protected_b64>.<payload_b64>" */
    size_t sb_len = strlen(out->protected_b64) + 1 + strlen(out->payload_b64) + 1;
    char *signing_input = (char *)malloc(sb_len);
    if (!signing_input) { jws_free(out); set_error("OOM signing_input"); return ESP_ERR_NO_MEM; }
    snprintf(signing_input, sb_len, "%s.%s", out->protected_b64, out->payload_b64);

    /* 5) SHA-256 + ECDSA sign (DER) */
    unsigned char hash[32];
    if (mbedtls_sha256((const unsigned char *)signing_input, sb_len - 1,
                       hash, 0) != 0) {
        free(signing_input); jws_free(out);
        set_error("sha256(signing_input) failed"); return ESP_FAIL;
    }
    unsigned char der_sig[MBEDTLS_ECDSA_MAX_LEN];
    size_t der_sig_len = 0;
    int rc = mbedtls_pk_sign(pk, MBEDTLS_MD_SHA256, hash, 32,
                             der_sig, sizeof(der_sig), &der_sig_len,
                             ml_acme_rng, NULL);
    free(signing_input);
    if (rc != 0) {
        jws_free(out);
        char errbuf[80];
        mbedtls_strerror(rc, errbuf, sizeof(errbuf));
        set_error("pk_sign: -0x%x (%s)", (unsigned)-rc, errbuf);
        return ESP_FAIL;
    }

    /* 6) DER → raw r||s (64 bytes) */
    unsigned char raw_sig[64];
    if (ecdsa_der_to_raw(der_sig, der_sig_len, raw_sig) != 0) {
        jws_free(out);
        set_error("ecdsa_der_to_raw parse failed");
        return ESP_FAIL;
    }

    /* 7) signature b64url */
    size_t sig_cap = ((64 + 2) / 3) * 4 + 4;
    out->signature_b64 = (char *)malloc(sig_cap);
    if (!out->signature_b64) { jws_free(out); set_error("OOM signature_b64"); return ESP_ERR_NO_MEM; }
    if (b64url_encode(raw_sig, 64, out->signature_b64, sig_cap) == 0) {
        jws_free(out); set_error("b64url(sig) failed"); return ESP_FAIL;
    }

    /* 8) JSON final: {"protected":"...","payload":"...","signature":"..."} */
    size_t jn = strlen(out->protected_b64) + strlen(out->payload_b64) +
                strlen(out->signature_b64) + 64;
    out->json = (char *)malloc(jn);
    if (!out->json) { jws_free(out); set_error("OOM jws json"); return ESP_ERR_NO_MEM; }
    out->json_len = (size_t)snprintf(out->json, jn,
        "{\"protected\":\"%s\",\"payload\":\"%s\",\"signature\":\"%s\"}",
        out->protected_b64, out->payload_b64, out->signature_b64);
    return ESP_OK;
}

/* ───────────────────────── HTTP request ──────────────────────────────
 * Devuelve ESP_OK si HTTP 2xx. Rellena *body (malloc'd, caller free) y
 * actualiza *cap (nonce, location) y, si new_nonce != NULL, también
 * s_last_nonce. */
static esp_err_t http_post(const char *url, const char *body_json,
                           int *out_status,
                           char **out_body, size_t *out_body_len,
                           http_capture_t *cap)
{
    *out_body = NULL;
    *out_body_len = 0;
    *out_status = 0;

    /* Pre-armamos JWS fuera del bucle de retry (la URL/nonce cambian por retry). */
    char *body_dup = body_json ? xstrdup(body_json) : NULL;
    if (body_json && !body_dup) {
        set_error("OOM body_dup");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t last_err = ESP_FAIL;
    for (int attempt = 0; attempt <= ML_ACME_HTTP_RETRY_MAX; attempt++) {
        /* Nuevo nonce si no tenemos uno (solo en primer intento). */
        const char *nonce = s_last_nonce[0] ? s_last_nonce : "";
        ESP_LOGI(TAG, "JWS attempt %d: nonce_len=%u nonce=%.12s url=%s",
                 attempt, (unsigned)strlen(nonce), nonce, url);

        jws_t jws;
        esp_err_t err = jws_build(&jws, &s_acct_pk, nonce, url, body_dup);
        if (err != ESP_OK) {
            free(body_dup);
            return err;
        }

        esp_http_client_config_t cfg = {
            .url = url,
            .method = HTTP_METHOD_POST,
            .timeout_ms = ML_ACME_HTTP_TIMEOUT_MS,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .event_handler = http_event_handler,
            .user_data = cap,
        };
        esp_http_client_handle_t h = esp_http_client_init(&cfg);
        if (!h) {
            jws_free(&jws);
            last_err = ESP_FAIL;
            set_error("http_client_init oom/url");
            continue;
        }
        esp_http_client_set_header(h, "Content-Type", "application/jose+json");

        esp_err_t open_err = esp_http_client_open(h, jws.json_len);
        if (open_err != ESP_OK) {
            set_error("http_open: %s", esp_err_to_name(open_err));
            esp_http_client_cleanup(h);
            jws_free(&jws);
            last_err = open_err;
            continue;
        }
        int w = esp_http_client_write(h, jws.json, jws.json_len);
        if (w < 0 || (size_t)w != jws.json_len) {
            set_error("http_write short: %d/%u", w, (unsigned)jws.json_len);
            esp_http_client_close(h);
            esp_http_client_cleanup(h);
            jws_free(&jws);
            last_err = ESP_FAIL;
            continue;
        }
        int fhdr = esp_http_client_fetch_headers(h);
        if (fhdr < 0) {
            set_error("fetch_headers: %d", fhdr);
            esp_http_client_close(h);
            esp_http_client_cleanup(h);
            jws_free(&jws);
            last_err = ESP_FAIL;
            continue;
        }
        int status = esp_http_client_get_status_code(h);
        *out_status = status;

        /* Acumular body. */
        size_t cap_sz = ML_ACME_RESP_INIT, used = 0;
        char *buf = (char *)malloc(cap_sz);
        if (!buf) {
            esp_http_client_close(h);
            esp_http_client_cleanup(h);
            jws_free(&jws);
            free(body_dup);
            set_error("OOM body buf");
            return ESP_ERR_NO_MEM;
        }
        int rd;
        while ((rd = esp_http_client_read(h, buf + used, cap_sz - used - 1)) > 0) {
            used += (size_t)rd;
            if (cap_sz - used < 256) {
                if (cap_sz >= ML_ACME_RESP_CAP) {
                    buf[used] = '\0';
                    ESP_LOGE(TAG, "response too big >%u", (unsigned)ML_ACME_RESP_CAP);
                    free(buf);
                    esp_http_client_close(h);
                    esp_http_client_cleanup(h);
                    jws_free(&jws);
                    free(body_dup);
                    set_error("response > %u bytes", (unsigned)ML_ACME_RESP_CAP);
                    return ESP_FAIL;
                }
                size_t new_cap = cap_sz * 2;
                if (new_cap > ML_ACME_RESP_CAP) new_cap = ML_ACME_RESP_CAP;
                char *nb = (char *)realloc(buf, new_cap);
                if (!nb) {
                    free(buf);
                    esp_http_client_close(h);
                    esp_http_client_cleanup(h);
                    jws_free(&jws);
                    free(body_dup);
                    set_error("OOM body grow");
                    return ESP_ERR_NO_MEM;
                }
                buf = nb;
                cap_sz = new_cap;
            }
        }
        buf[used] = '\0';
        esp_http_client_close(h);
        esp_http_client_cleanup(h);
        jws_free(&jws);

        /* Guardar nonce nuevo para próximas requests. */
        if (cap->got_nonce && cap->replay_nonce[0]) {
            strncpy(s_last_nonce, cap->replay_nonce, sizeof(s_last_nonce) - 1);
            s_last_nonce[sizeof(s_last_nonce) - 1] = '\0';
            ESP_LOGI(TAG, "s_last_nonce updated len=%u", (unsigned)strlen(s_last_nonce));
        }

        if (status >= 200 && status < 300) {
            *out_body = buf;
            *out_body_len = used;
            free(body_dup);
            return ESP_OK;
        }

        /* 4xx "badNonce" → retry. */
        bool bad_nonce = false;
        if (status == 400 || status == 403) {
            if (used > 0) {
                cJSON *j = cJSON_Parse(buf);
                /* El problem document ACME va en top-level:
                 * {"type":"urn:ietf:params:acme:error:badNonce","detail":...} */
                const cJSON *t = j ? cJSON_GetObjectItemCaseSensitive(j, "type") : NULL;
                if (cJSON_IsString(t) && t->valuestring &&
                    strstr(t->valuestring, "badNonce")) {
                    bad_nonce = true;
                }
                cJSON_Delete(j);
            }
        }
        free(buf);

        if (bad_nonce && attempt < ML_ACME_HTTP_RETRY_MAX) {
            ESP_LOGW(TAG, "badNonce (status %d), retrying with fresh nonce", status);
            /* s_last_nonce ya fue actualizado con el Replay-Nonce de esta
             * respuesta 400 — NO limpiarlo: el retry debe usarlo. */
            continue;
        }

        /* Si no fue badNonce o ya no quedan reintentos. Loggear el PROBLEMA
         * de respuesta (body ACME) no el request. */
        ESP_LOGE(TAG, "HTTP %d url=%s used=%u nonce_len=%u", status, url,
                 (unsigned)used, (unsigned)strlen(s_last_nonce));
        if (used > 0) {
            ESP_LOGE(TAG, "problem: %.*s", (int)(used > 400 ? 400 : used), buf);
            ESP_LOGE(TAG, "body hex first 32: %02x %02x %02x %02x %02x %02x %02x %02x",
                     buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
            cJSON *j = cJSON_Parse(buf);
            const cJSON *dt = j ? cJSON_GetObjectItemCaseSensitive(j, "detail") : NULL;
            if (cJSON_IsString(dt) && dt->valuestring) {
                snprintf(s_last_error, sizeof(s_last_error), "HTTP %d: %s", status, dt->valuestring);
            } else {
                snprintf(s_last_error, sizeof(s_last_error), "HTTP %d at %s", status, url);
            }
            cJSON_Delete(j);
        } else {
            snprintf(s_last_error, sizeof(s_last_error), "HTTP %d (no body) at %s", status, url);
        }
        last_err = ESP_FAIL;
        break;
    }

    free(body_dup);
    return last_err;
}

/* POST-as-GET: payload es string vacío. */
static esp_err_t http_post_as_get(const char *url, int *out_status,
                                  char **out_body, size_t *out_body_len,
                                  http_capture_t *cap)
{
    return http_post(url, NULL, out_status, out_body, out_body_len, cap);
}

/* GET plain (sin JWS). Se usa para el directory y para newNonce.
 * Captura nonce vía el event handler igual que POST. */
static esp_err_t http_get(const char *url, int *out_status,
                          char **out_body, size_t *out_body_len,
                          http_capture_t *cap)
{
    *out_body = NULL; *out_body_len = 0; *out_status = 0;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = ML_ACME_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event_handler,
        .user_data = cap,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) { set_error("http_get init"); return ESP_FAIL; }

    esp_err_t err = esp_http_client_open(h, 0);
    if (err != ESP_OK) {
        set_error("http_get open: %s", esp_err_to_name(err));
        esp_http_client_cleanup(h);
        return err;
    }
    int fhdr = esp_http_client_fetch_headers(h);
    if (fhdr < 0) {
        set_error("http_get fetch_headers: %d", fhdr);
        esp_http_client_close(h);
        esp_http_client_cleanup(h);
        return ESP_FAIL;
    }
    int status = esp_http_client_get_status_code(h);
    *out_status = status;

    size_t cap_sz = ML_ACME_RESP_INIT, used = 0;
    char *buf = (char *)malloc(cap_sz);
    if (!buf) {
        esp_http_client_close(h);
        esp_http_client_cleanup(h);
        set_error("OOM get body");
        return ESP_ERR_NO_MEM;
    }
    int rd;
    while ((rd = esp_http_client_read(h, buf + used, cap_sz - used - 1)) > 0) {
        used += (size_t)rd;
        if (cap_sz - used < 256 && cap_sz < ML_ACME_RESP_CAP) {
            size_t new_cap = cap_sz * 2;
            if (new_cap > ML_ACME_RESP_CAP) new_cap = ML_ACME_RESP_CAP;
            char *nb = (char *)realloc(buf, new_cap);
            if (!nb) {
                free(buf);
                esp_http_client_close(h);
                esp_http_client_cleanup(h);
                set_error("OOM get grow");
                return ESP_ERR_NO_MEM;
            }
            buf = nb; cap_sz = new_cap;
        }
    }
    buf[used] = '\0';
    esp_http_client_close(h);
    esp_http_client_cleanup(h);

    if (status < 200 || status >= 300) {
        free(buf);
        set_error("HTTP %d GET %s", status, url);
        return ESP_FAIL;
    }
    *out_body = buf;
    *out_body_len = used;
    return ESP_OK;
}

/* ───────────────────────── Directory ──────────────────────────────── */

typedef struct {
    char new_nonce[256];
    char new_account[256];
    char new_order[256];
    char revoke[256];
    /* finalize puede venir en order body, pero algunos servers lo incluyen
     * aquí también (Let's Encrypt staging lo hace). */
    char finalize[256];
} acme_directory_t;

static esp_err_t fetch_directory(acme_directory_t *d)
{
    progress("directory", ESP_OK);
    memset(d, 0, sizeof(*d));
    int status = 0;
    char *body = NULL;
    size_t blen = 0;
    http_capture_t cap = {0};
    /* El directory endpoint de Let's Encrypt acepta GET plano. */
    esp_err_t err = http_get(s_directory_url, &status, &body, &blen, &cap);
    if (err != ESP_OK) {
        progress("directory", err);
        free(body);
        return err;
    }
    if (status != 200) {
        free(body);
        progress("directory", ESP_FAIL);
        set_error("directory HTTP %d", status);
        return ESP_FAIL;
    }
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) {
        progress("directory", ESP_FAIL);
        set_error("directory parse");
        return ESP_FAIL;
    }
    const cJSON *nn = cJSON_GetObjectItemCaseSensitive(j, "newNonce");
    const cJSON *na = cJSON_GetObjectItemCaseSensitive(j, "newAccount");
    const cJSON *no = cJSON_GetObjectItemCaseSensitive(j, "newOrder");
    const cJSON *rv = cJSON_GetObjectItemCaseSensitive(j, "revoke");
    const cJSON *fn = cJSON_GetObjectItemCaseSensitive(j, "finalize");
    if (cJSON_IsString(nn)) strncpy(d->new_nonce,    nn->valuestring, sizeof(d->new_nonce)    - 1);
    if (cJSON_IsString(na)) strncpy(d->new_account, na->valuestring, sizeof(d->new_account) - 1);
    if (cJSON_IsString(no)) strncpy(d->new_order,   no->valuestring, sizeof(d->new_order)   - 1);
    if (cJSON_IsString(rv)) strncpy(d->revoke,      rv->valuestring, sizeof(d->revoke)      - 1);
    if (cJSON_IsString(fn)) strncpy(d->finalize,    fn->valuestring, sizeof(d->finalize)    - 1);
    cJSON_Delete(j);
    if (d->new_nonce[0] == '\0' || d->new_account[0] == '\0' ||
        d->new_order[0] == '\0') {
        progress("directory", ESP_FAIL);
        set_error("directory missing fields");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ───────────────────────── newAccount ───────────────────────────────── */

static esp_err_t do_new_account(const char *new_account_url, const char *hostname)
{
    progress("newAccount", ESP_OK);
    /* LE exige un contacto con dominio publico valido. Si no hay email
     * configurado (ML_ACME_CONTACT_EMAIL vacio), se omite el campo. */
    char payload[320];
    const char *contact = CONFIG_ML_ACME_CONTACT_EMAIL;
    if (contact[0] != '\0') {
        snprintf(payload, sizeof(payload),
                 "{\"termsOfServiceAgreed\":true,\"contact\":[\"mailto:%s\"]}",
                 contact);
    } else {
        snprintf(payload, sizeof(payload), "{\"termsOfServiceAgreed\":true}");
    }

    int status = 0;
    char *body = NULL;
    size_t blen = 0;
    http_capture_t cap = {0};
    esp_err_t err = http_post(new_account_url, payload, &status, &body, &blen, &cap);
    if (err != ESP_OK) {
        progress("newAccount", err);
        return err;
    }
    if (status != 201 && status != 200) {
        progress("newAccount", ESP_FAIL);
        free(body);
        set_error("newAccount HTTP %d", status);
        return ESP_FAIL;
    }
    /* Account URL: preferir header Location; caer a body.id. */
    if (cap.got_location && cap.location[0]) {
        strncpy(s_kid, cap.location, sizeof(s_kid) - 1);
        s_kid[sizeof(s_kid) - 1] = '\0';
    } else {
        cJSON *j = cJSON_Parse(body);
        if (j) {
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(j, "id");
            if (cJSON_IsString(id)) {
                strncpy(s_kid, id->valuestring, sizeof(s_kid) - 1);
                s_kid[sizeof(s_kid) - 1] = '\0';
            }
            cJSON_Delete(j);
        }
    }
    free(body);
    if (s_kid[0] == '\0') {
        progress("newAccount", ESP_FAIL);
        set_error("newAccount: no kid");
        return ESP_FAIL;
    }

    /* Persistir kid en NVS (asociado a acct_key). */
    nvs_handle_t nvs;
    if (nvs_open(ML_ACME_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, ML_ACME_NVS_KID, s_kid);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "newAccount ok, kid=%s", s_kid);
    (void)hostname;
    return ESP_OK;
}

/* ───────────────────────── newOrder ────────────────────────────────── */

static esp_err_t do_new_order(const char *new_order_url, const char *hostname,
                              char *out_order_url, size_t cap,
                              char *out_authz_url, size_t cap2,
                              char *out_finalize_url, size_t cap3)
{
    progress("newOrder", ESP_OK);
    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"identifiers\":[{\"type\":\"dns\",\"value\":\"%s\"}]}", hostname);

    int status = 0;
    char *body = NULL;
    size_t blen = 0;
    http_capture_t cap_c = {0};
    esp_err_t err = http_post(new_order_url, payload, &status, &body, &blen, &cap_c);
    if (err != ESP_OK) {
        progress("newOrder", err);
        return err;
    }
    if (status != 201 && status != 200) {
        progress("newOrder", ESP_FAIL);
        free(body);
        set_error("newOrder HTTP %d", status);
        return ESP_FAIL;
    }
    /* Order URL via Location si lo da (LE lo hace en 201). */
    if (cap_c.got_location && cap_c.location[0]) {
        strncpy(out_order_url, cap_c.location, cap - 1);
        out_order_url[cap - 1] = '\0';
    }
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) {
        progress("newOrder", ESP_FAIL);
        set_error("newOrder parse");
        return ESP_FAIL;
    }
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(j, "id");
    if (cJSON_IsString(id) && out_order_url[0] == '\0') {
        strncpy(out_order_url, id->valuestring, cap - 1);
        out_order_url[cap - 1] = '\0';
    }
    const cJSON *auths = cJSON_GetObjectItemCaseSensitive(j, "authorizations");
    if (cJSON_IsArray(auths) && cJSON_GetArraySize(auths) > 0) {
        const cJSON *a = cJSON_GetArrayItem(auths, 0);
        if (cJSON_IsString(a)) {
            strncpy(out_authz_url, a->valuestring, cap2 - 1);
            out_authz_url[cap2 - 1] = '\0';
        }
    }
    const cJSON *fn = cJSON_GetObjectItemCaseSensitive(j, "finalize");
    if (cJSON_IsString(fn)) {
        strncpy(out_finalize_url, fn->valuestring, cap3 - 1);
        out_finalize_url[cap3 - 1] = '\0';
    }
    cJSON_Delete(j);
    if (out_order_url[0] == '\0' || out_authz_url[0] == '\0' ||
        out_finalize_url[0] == '\0') {
        progress("newOrder", ESP_FAIL);
        set_error("newOrder fields missing");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "newOrder ok: %s", out_order_url);
    return ESP_OK;
}

/* ───────────────────────── Authorizations / Challenge ──────────────── */

static esp_err_t do_get_authz(const char *authz_url,
                              char *out_challenge_url, size_t cap_ch,
                              char *out_challenge_token, size_t cap_tok,
                              char *out_status, size_t cap_st)
{
    int status = 0;
    char *body = NULL;
    size_t blen = 0;
    http_capture_t cap_c = {0};
    esp_err_t err = http_post_as_get(authz_url, &status, &body, &blen, &cap_c);
    if (err != ESP_OK) return err;
    if (status != 200 && status != 201) {
        free(body);
        set_error("authz HTTP %d", status);
        return ESP_FAIL;
    }
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) {
        set_error("authz parse");
        return ESP_FAIL;
    }
    const cJSON *st = cJSON_GetObjectItemCaseSensitive(j, "status");
    if (cJSON_IsString(st)) {
        strncpy(out_status, st->valuestring, cap_st - 1);
        out_status[cap_st - 1] = '\0';
    }
    const cJSON *ch = cJSON_GetObjectItemCaseSensitive(j, "challenges");
    if (cJSON_IsArray(ch)) {
        cJSON *item;
        cJSON_ArrayForEach(item, ch) {
            const cJSON *tp = cJSON_GetObjectItemCaseSensitive(item, "type");
            if (cJSON_IsString(tp) && strcmp(tp->valuestring, "dns-01") == 0) {
                const cJSON *ur = cJSON_GetObjectItemCaseSensitive(item, "url");
                const cJSON *tok = cJSON_GetObjectItemCaseSensitive(item, "token");
                if (cJSON_IsString(ur)) {
                    strncpy(out_challenge_url, ur->valuestring, cap_ch - 1);
                    out_challenge_url[cap_ch - 1] = '\0';
                }
                if (cJSON_IsString(tok)) {
                    strncpy(out_challenge_token, tok->valuestring, cap_tok - 1);
                    out_challenge_token[cap_tok - 1] = '\0';
                }
                break;
            }
        }
    }
    cJSON_Delete(j);
    if (out_challenge_url[0] == '\0' || out_challenge_token[0] == '\0') {
        set_error("authz missing dns-01 challenge");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t do_post_challenge(const char *challenge_url)
{
    progress("challenge", ESP_OK);
    /* Payload = "{}" (objeto vacío). */
    int status = 0;
    char *body = NULL;
    size_t blen = 0;
    http_capture_t cap_c = {0};
    esp_err_t err = http_post(challenge_url, "{}", &status, &body, &blen, &cap_c);
    if (err != ESP_OK) {
        progress("challenge", err);
        return err;
    }
    free(body);
    /* LE devuelve 200 OK con status="pending". */
    if (status < 200 || status >= 300) {
        progress("challenge", ESP_FAIL);
        set_error("challenge POST HTTP %d", status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t poll_authz(const char *authz_url)
{
    progress("poll-authz", ESP_OK);
    int waited = 0;
    char status[32];
    char challenge_url[512], challenge_token[128];
    while (waited < ML_ACME_POLL_AUTHZ_MAX_MS) {
        vTaskDelay(pdMS_TO_TICKS(ML_ACME_POLL_AUTHZ_MS));
        waited += ML_ACME_POLL_AUTHZ_MS;
        status[0] = '\0';
        challenge_url[0] = '\0';
        challenge_token[0] = '\0';
        esp_err_t err = do_get_authz(authz_url,
                                      challenge_url, sizeof(challenge_url),
                                      challenge_token, sizeof(challenge_token),
                                      status, sizeof(status));
        if (err != ESP_OK) {
            /* Reintentar hasta agotar la ventana; reportar al final. */
            ESP_LOGW(TAG, "poll-authz err=%s (waited=%d)", s_last_error, waited);
            continue;
        }
        ESP_LOGI(TAG, "authz status=%s (%d ms)", status, waited);
        if (strcmp(status, "valid") == 0) {
            return ESP_OK;
        }
        if (strcmp(status, "invalid") == 0) {
            progress("poll-authz", ESP_FAIL);
            set_error("authz invalid");
            return ESP_FAIL;
        }
    }
    progress("poll-authz", ESP_FAIL);
    set_error("authz timeout");
    return ESP_FAIL;
}

/* ───────────────────────── CSR ────────────────────────────────────────
 * Construimos SAN DER a mano:
 *   SubjectAltName ::= SEQUENCE SIZE (1..MAX) OF GeneralName
 *   GeneralName ::= CHOICE { ... [2] dNSName IA5String }
 *   GeneralNames ::= SEQUENCE SIZE (1..MAX) OF GeneralName
 *
 * Por tanto:
 *   SEQUENCE {
 *     [2] IA5String "hostname"
 *   }
 * Codificado:
 *   0x30 <total_len>           (SEQUENCE outer)
 *   0x82 <hi> <lo>             (context specific [2] constructed; long form len)
 *   <strlen>                   (IA5String length)
 *   "hostname"                 (bytes del host)
 *
 * Si hostname > 127 bytes, el long-form len son 2 bytes; si ≤127, short form.
 */
static esp_err_t build_san_der(const char *hostname, unsigned char *out, size_t *out_len)
{
    if (!hostname) return ESP_ERR_INVALID_ARG;
    size_t hn = strlen(hostname);
    if (hn == 0 || hn > 127) {
        /* Para simplicidad soportamos hostnames de hasta 127 bytes; los FQDN
         * Tailscale reales son <100. Esto cabe en DER short-form length. */
        return ESP_ERR_INVALID_ARG;
    }

    /* Estructura correcta (SubjectAltName extension value, RFC 5280):
     *
     *   SEQUENCE {                 [0x30 L1]
     *     [2] dNSName PRIMITIVE    [0x82 L2]  <- tag [2] IMPLICIT, sin TLV interno
     *       <hostname bytes>
     *     }
     *
     * dNSName es context-tag [2] PRIMITIVO: el valor son los bytes IA5 del
     * hostname DIRECTAMENTE (no se envuelve en un IA5String 0x16).
     * L2 = hn, L1 = hn + 2. Tamaño total = hn + 4.
     */
    size_t idx = 0;
    out[idx++] = 0x30;                  /* SEQUENCE */
    out[idx++] = (unsigned char)(hn + 2); /* L1 */
    out[idx++] = 0x82;                  /* [2] dNSName, primitive */
    out[idx++] = (unsigned char)hn;     /* L2 */
    memcpy(out + idx, hostname, hn);
    idx += hn;
    *out_len = idx;
    return ESP_OK;
}

static esp_err_t build_csr_der(const char *hostname, unsigned char **out_der,
                               size_t *out_der_len, char **out_pem)
{
    *out_der = NULL; *out_der_len = 0; *out_pem = NULL;
    esp_err_t ret_ok = ESP_FAIL;

    unsigned char san_buf[300];
    size_t san_len = 0;
    if (build_san_der(hostname, san_buf, &san_len) != ESP_OK) {
        set_error("build_san_der failed");
        return ESP_FAIL;
    }

    mbedtls_x509write_csr csr;
    mbedtls_x509write_csr_init(&csr);

    char subject_name[300];
    snprintf(subject_name, sizeof(subject_name), "CN=%s", hostname);
    int rc = mbedtls_x509write_csr_set_subject_name(&csr, subject_name);
    if (rc != 0) { set_error("csr set_subject_name: -0x%x", (unsigned)-rc); goto cleanup; }
    mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);
    rc = mbedtls_x509write_csr_set_extension(&csr,
            MBEDTLS_OID_SUBJECT_ALT_NAME,
            MBEDTLS_OID_SIZE(MBEDTLS_OID_SUBJECT_ALT_NAME),
            0, san_buf, san_len);
    if (rc != 0) { set_error("csr set_ext SAN: -0x%x", (unsigned)-rc); goto cleanup; }
    mbedtls_x509write_csr_set_key(&csr, &s_leaf_pk);

    /* Output DER. Necesita buffer suficientemente grande (típico <512B). */
    size_t buf_sz = 1024;
    unsigned char *buf = (unsigned char *)malloc(buf_sz);
    if (!buf) { set_error("OOM csr buf"); goto cleanup; }
    int n = mbedtls_x509write_csr_der(&csr, buf, buf_sz,
                                      ml_acme_rng, NULL);
    if (n < 0) {
        free(buf);
        set_error("csr_der: -0x%x", (unsigned)-n);
        goto cleanup;
    }
    /* mbedtls_x509write_csr_der escribe el DER ALINEADO AL FINAL del buffer
     * (retorna la longitud). El DER real arranca en buf + buf_sz - n. */
    *out_der_len = (size_t)n;
    *out_der = (unsigned char *)malloc(*out_der_len);
    if (!*out_der) { free(buf); set_error("OOM csr der copy"); goto cleanup; }
    memcpy(*out_der, buf + buf_sz - n, *out_der_len);

    /* También emitimos PEM por conveniencia (lo pasamos a ml_cert_store). */
    size_t pem_cap = 512;
    char *pem = (char *)malloc(pem_cap);
    if (!pem) { free(buf); set_error("OOM csr pem"); goto cleanup; }
    n = mbedtls_x509write_csr_pem(&csr, (unsigned char *)pem, pem_cap,
                                  ml_acme_rng, NULL);
    if (n != 0) {
        free(buf); free(pem);
        set_error("csr_pem: -0x%x", (unsigned)-n);
        goto cleanup;
    }
    *out_pem = pem;

    free(buf);
    ret_ok = ESP_OK;

cleanup:
    mbedtls_x509write_csr_free(&csr);
    return ret_ok;
}

/* ───────────────────────── finalize + poll order + cert ────────────── */

static esp_err_t do_finalize(const char *finalize_url, const char *hostname)
{
    progress("finalize", ESP_OK);
    unsigned char *der = NULL; size_t der_len = 0;
    char *pem_unused = NULL;
    esp_err_t err = build_csr_der(hostname, &der, &der_len, &pem_unused);
    free(pem_unused);
    if (err != ESP_OK) { progress("finalize", ESP_FAIL); return err; }

    char *csr_b64 = (char *)malloc(((der_len + 2) / 3) * 4 + 4);
    if (!csr_b64) { free(der); progress("finalize", ESP_FAIL); set_error("OOM csr_b64"); return ESP_ERR_NO_MEM; }
    if (b64url_encode(der, der_len, csr_b64, ((der_len + 2) / 3) * 4 + 4) == 0) {
        free(der); free(csr_b64);
        progress("finalize", ESP_FAIL);
        set_error("b64url(csr) failed");
        return ESP_FAIL;
    }
    free(der);

    size_t pl_cap = strlen(csr_b64) + 32;
    char *payload = (char *)malloc(pl_cap);
    if (!payload) { free(csr_b64); progress("finalize", ESP_FAIL); set_error("OOM finalize payload"); return ESP_ERR_NO_MEM; }
    snprintf(payload, pl_cap, "{\"csr\":\"%s\"}", csr_b64);
    free(csr_b64);

    int status = 0;
    char *body = NULL; size_t blen = 0;
    http_capture_t cap_c = {0};
    err = http_post(finalize_url, payload, &status, &body, &blen, &cap_c);
    free(payload);
    if (err != ESP_OK) { progress("finalize", err); free(body); return err; }
    free(body);
    if (status < 200 || status >= 300) {
        progress("finalize", ESP_FAIL);
        set_error("finalize HTTP %d", status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t poll_order(const char *order_url, char *out_cert_url, size_t cap)
{
    progress("poll-order", ESP_OK);
    int waited = 0;
    while (waited < ML_ACME_POLL_ORDER_MAX_MS) {
        vTaskDelay(pdMS_TO_TICKS(ML_ACME_POLL_ORDER_MS));
        waited += ML_ACME_POLL_ORDER_MS;
        int status = 0;
        char *body = NULL; size_t blen = 0;
        http_capture_t cap_c = {0};
        esp_err_t err = http_post_as_get(order_url, &status, &body, &blen, &cap_c);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "poll-order err=%s", s_last_error);
            free(body);
            continue;
        }
        if (status != 200 && status != 201) {
            ESP_LOGW(TAG, "poll-order HTTP %d", status);
            free(body); continue;
        }
        cJSON *j = cJSON_Parse(body);
        free(body);
        if (!j) continue;
        const cJSON *st = cJSON_GetObjectItemCaseSensitive(j, "status");
        char sval[32] = {0};
        if (cJSON_IsString(st)) {
            strncpy(sval, st->valuestring, sizeof(sval) - 1);
        }
        ESP_LOGI(TAG, "order status=%s (%d ms)", sval, waited);
        if (strcmp(sval, "valid") == 0) {
            const cJSON *cu = cJSON_GetObjectItemCaseSensitive(j, "certificate");
            if (cJSON_IsString(cu)) {
                strncpy(out_cert_url, cu->valuestring, cap - 1);
                out_cert_url[cap - 1] = '\0';
            }
            cJSON_Delete(j);
            if (out_cert_url[0]) return ESP_OK;
            set_error("order valid but no certificate URL");
            return ESP_FAIL;
        }
        if (strcmp(sval, "invalid") == 0) {
            char *jdump = cJSON_PrintUnformatted(j);
            ESP_LOGE(TAG, "order invalid: %s", jdump ? jdump : "?");
            free(jdump);
            cJSON_Delete(j);
            progress("poll-order", ESP_FAIL);
            set_error("order invalid");
            return ESP_FAIL;
        }
        cJSON_Delete(j);
    }
    progress("poll-order", ESP_FAIL);
    set_error("order poll timeout");
    return ESP_FAIL;
}

static esp_err_t do_download_cert(const char *cert_url,
                                  uint8_t **out_fullchain, size_t *out_len)
{
    progress("certificate", ESP_OK);
    int status = 0;
    char *body = NULL; size_t blen = 0;
    http_capture_t cap_c = {0};
    esp_err_t err = http_post_as_get(cert_url, &status, &body, &blen, &cap_c);
    if (err != ESP_OK) { progress("certificate", err); free(body); return err; }
    if (status != 200) {
        free(body);
        progress("certificate", ESP_FAIL);
        set_error("cert HTTP %d", status);
        return ESP_FAIL;
    }
    *out_fullchain = (uint8_t *)body;   /* transferimos ownership */
    *out_len = blen;
    return ESP_OK;
}

/* ───────────────────────── Helpers varios ──────────────────────────── */

static void progress(const char *stage, esp_err_t err)
{
    if (s_progress) s_progress(stage, err);
}

static void set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_last_error, sizeof(s_last_error), fmt, ap);
    va_end(ap);
}

/* RNG shim para mbedtls: usa el TRNG del ESP32 (no bloqueante). */
static int ml_acme_rng(void *ctx, unsigned char *buf, size_t len)
{
    (void)ctx;
    esp_fill_random(buf, len);
    return 0;
}

/* ───────────────────────── API pública ─────────────────────────────── */

esp_err_t ml_acme_set_directory(const char *url)
{
    if (!url) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs;
    if (nvs_open(ML_ACME_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        /* Si el directorio CAMBIA (staging -> produccion), la account kid y
         * el last_issue previos son de otra CA: invalidarlos para forzar
         * newAccount + nueva emision. */
        char prev[256] = {0};
        size_t plen = sizeof(prev);
        bool changed = (nvs_get_str(nvs, ML_ACME_NVS_DIR_OVERRIDE, prev, &plen) == ESP_OK &&
                        strlen(prev) > 0 && strcmp(prev, url) != 0);
        if (changed) {
            ESP_LOGI(TAG, "directory changed -> invalidating kid + last_issue");
            nvs_erase_key(nvs, ML_ACME_NVS_KID);
            nvs_erase_key(nvs, ML_ACME_NVS_LAST_ISSUE);
            s_kid[0] = '\0';
        }
        strncpy(s_directory_url, url, sizeof(s_directory_url) - 1);
        s_directory_url[sizeof(s_directory_url) - 1] = '\0';
        nvs_set_str(nvs, ML_ACME_NVS_DIR_OVERRIDE, s_directory_url);
        nvs_commit(nvs);
        nvs_close(nvs);
    } else {
        strncpy(s_directory_url, url, sizeof(s_directory_url) - 1);
        s_directory_url[sizeof(s_directory_url) - 1] = '\0';
    }
    return ESP_OK;
}

esp_err_t ml_acme_set_dns_publisher(ml_acme_set_dns_fn fn)
{
    s_set_dns = fn;
    return ESP_OK;
}

esp_err_t ml_acme_set_progress_cb(ml_acme_progress_fn cb)
{
    s_progress = cb;
    return ESP_OK;
}

esp_err_t ml_acme_init(void)
{
    if (s_keys_loaded) return ESP_OK;

    esp_err_t err;
    err = load_or_gen_key(ML_ACME_NVS_ACCT_KEY, &s_acct_pk);
    if (err != ESP_OK) return err;
    err = load_or_gen_key(ML_ACME_NVS_LEAF_KEY, &s_leaf_pk);
    if (err != ESP_OK) {
        mbedtls_pk_free(&s_acct_pk);
        return err;
    }
    s_keys_loaded = true;

    /* Cargar kid persistido (si ya teníamos cuenta) y override de directorio. */
    nvs_handle_t nvs;
    if (nvs_open(ML_ACME_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(s_kid);
        if (nvs_get_str(nvs, ML_ACME_NVS_KID, s_kid, &len) != ESP_OK) {
            s_kid[0] = '\0';
        }
        len = sizeof(s_directory_url);
        char tmp[256];
        if (nvs_get_str(nvs, ML_ACME_NVS_DIR_OVERRIDE, tmp, &len) == ESP_OK) {
            strncpy(s_directory_url, tmp, sizeof(s_directory_url) - 1);
            s_directory_url[sizeof(s_directory_url) - 1] = '\0';
        }
        nvs_close(nvs);
    }
    return ESP_OK;
}

esp_err_t ml_acme_obtain_cert(const char *hostname)
{
    if (!hostname) { set_error("hostname NULL"); return ESP_ERR_INVALID_ARG; }
    if (!s_keys_loaded) {
        set_error("ml_acme_init not called");
        return ESP_FAIL;
    }
    if (!s_set_dns) {
        set_error("dns publisher not set");
        return ESP_FAIL;
    }

    esp_err_t err;
    char order_url[512] = {0};
    char authz_url[512] = {0};
    char finalize_url[512] = {0};
    char cert_url[512] = {0};
    char challenge_url[512] = {0};
    char challenge_token[128] = {0};
    char status_buf[32] = {0};
    uint8_t *fullchain = NULL; size_t fclen = 0;

    /* 1) Directory */
    acme_directory_t dir;
    err = fetch_directory(&dir);
    if (err != ESP_OK) goto out;

    /* Asegurar nonce fresco. */
    if (s_last_nonce[0] == '\0') {
        progress("nonce", ESP_OK);
        int st = 0; char *b = NULL; size_t bl = 0; http_capture_t c = {0};
        /* newNonce: LE acepta GET o HEAD (devuelve 204 con nonce en Replay-Nonce). */
        err = http_get(dir.new_nonce, &st, &b, &bl, &c);
        free(b);
        if (err != ESP_OK || st != 204) {
            progress("nonce", ESP_FAIL);
            set_error("newNonce HTTP %d", st);
            goto out;
        }
        if (c.got_nonce && c.replay_nonce[0]) {
            strncpy(s_last_nonce, c.replay_nonce, sizeof(s_last_nonce) - 1);
            s_last_nonce[sizeof(s_last_nonce) - 1] = '\0';
            ESP_LOGI(TAG, "fresh nonce from newNonce endpoint (len=%u)",
                     (unsigned)strlen(s_last_nonce));
        } else {
            set_error("newNonce: no Replay-Nonce header");
            goto out;
        }
    }

    /* 2) newAccount */
    if (s_kid[0] == '\0') {
        err = do_new_account(dir.new_account, hostname);
        if (err != ESP_OK) goto out;
    } else {
        ESP_LOGI(TAG, "reusing kid=%s", s_kid);
    }

    /* 3) newOrder */
    err = do_new_order(dir.new_order, hostname,
                       order_url, sizeof(order_url),
                       authz_url, sizeof(authz_url),
                       finalize_url, sizeof(finalize_url));
    if (err != ESP_OK) goto out;

    /* 4) authz → challenge dns-01 */
    progress("authz", ESP_OK);
    err = do_get_authz(authz_url,
                       challenge_url, sizeof(challenge_url),
                       challenge_token, sizeof(challenge_token),
                       status_buf, sizeof(status_buf));
    if (err != ESP_OK) { progress("authz", ESP_FAIL); goto out; }

    /* 5) JWK thumbprint para keyauth */
    char *thumbprint = NULL;
    {
        char *canon = build_jwk_canonical_json(&s_acct_pk);
        if (!canon) { set_error("build_jwk_canonical"); err = ESP_FAIL; goto out; }
        char tb[64];
        if (sha256_b64url((const unsigned char *)canon, strlen(canon), tb, sizeof(tb)) < 0) {
            free(canon); set_error("thumbprint sha256"); err = ESP_FAIL; goto out;
        }
        free(canon);
        thumbprint = xstrdup(tb);
        if (!thumbprint) { set_error("OOM thumbprint"); err = ESP_ERR_NO_MEM; goto out; }
    }

    /* keyauthz = token "." thumbprint */
    size_t klen = strlen(challenge_token) + 1 + strlen(thumbprint) + 1;
    char *keyauthz = (char *)malloc(klen);
    if (!keyauthz) { free(thumbprint); set_error("OOM keyauthz"); err = ESP_ERR_NO_MEM; goto out; }
    snprintf(keyauthz, klen, "%s.%s", challenge_token, thumbprint);
    free(thumbprint);

    /* txt = b64url(sha256(keyauthz)) SIN padding. */
    char txt[64];
    if (sha256_b64url((const unsigned char *)keyauthz, strlen(keyauthz),
                      txt, sizeof(txt)) < 0) {
        free(keyauthz); set_error("sha256(keyauthz)"); err = ESP_FAIL; goto out;
    }
    char *name_buf = (char *)malloc(strlen(hostname) + 32);
    if (!name_buf) { free(keyauthz); set_error("OOM name_buf"); err = ESP_ERR_NO_MEM; goto out; }
    snprintf(name_buf, strlen(hostname) + 32, "_acme-challenge.%s", hostname);

    /* 6) Publicar TXT */
    progress("set-dns", ESP_OK);
    esp_err_t pub_err = s_set_dns(name_buf, txt, ML_ACME_PUBLISH_TIMEOUT_MS);
    free(name_buf);
    if (pub_err != ESP_OK) {
        progress("set-dns", pub_err);
        free(keyauthz);
        set_error("dns publisher: %s", esp_err_to_name(pub_err));
        err = ESP_FAIL;
        goto out;
    }

    /* 7) Esperar propagación. */
    vTaskDelay(pdMS_TO_TICKS(ML_ACME_PROPAGATION_MS));

    /* 8) POST challenge */
    err = do_post_challenge(challenge_url);
    free(keyauthz);
    if (err != ESP_OK) goto out;

    /* 9) Poll authz */
    err = poll_authz(authz_url);
    if (err != ESP_OK) goto out;

    /* 10) finalize */
    err = do_finalize(finalize_url, hostname);
    if (err != ESP_OK) goto out;

    /* 11) Poll order */
    err = poll_order(order_url, cert_url, sizeof(cert_url));
    if (err != ESP_OK) goto out;

    /* 12) Download cert */
    err = do_download_cert(cert_url, &fullchain, &fclen);
    if (err != ESP_OK) goto out;

    /* 13) Persistir. Generamos PEM de la leaf key y llamamos ml_cert_store. */
    {
        size_t pem_cap = 512;
        unsigned char *leaf_pem = (unsigned char *)malloc(pem_cap);
        if (!leaf_pem) {
            free(fullchain);
            set_error("OOM leaf_pem");
            err = ESP_ERR_NO_MEM;
            goto out;
        }
        int n = mbedtls_pk_write_key_pem(&s_leaf_pk, leaf_pem, pem_cap);
        if (n != 0) {
            free(leaf_pem); free(fullchain);
            set_error("pk_write_key_pem: -0x%x", (unsigned)-n);
            err = ESP_FAIL;
            goto out;
        }
        /* pk_write_key_pem retorna 0 en exito; la longitud es strlen (PEM
         * null-terminated). Pasar 0 invalidaba ml_cert_store. */
        size_t key_pem_len = strlen((char *)leaf_pem);
        ESP_LOGI(TAG, "storing cert: fullchain=%u bytes, key=%u bytes",
                 (unsigned)fclen, (unsigned)key_pem_len);
        {
            /* DEBUG: hexdump de los últimos 40 bytes y scan de nulls internos */
            unsigned char *fc = fullchain;
            ESP_LOGI(TAG, "fc tail hex: %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x",
                     fc[fclen-16],fc[fclen-15],fc[fclen-14],fc[fclen-13],
                     fc[fclen-12],fc[fclen-11],fc[fclen-10],fc[fclen-9],
                     fc[fclen-8],fc[fclen-7],fc[fclen-6],fc[fclen-5],
                     fc[fclen-4],fc[fclen-3],fc[fclen-2],fc[fclen-1]);
            for (size_t zi = 0; zi < fclen; zi++) {
                if (fc[zi] == 0) {
                    ESP_LOGW(TAG, "NULL byte inside fullchain at offset %u of %u",
                             (unsigned)zi, (unsigned)fclen);
                    break;
                }
            }
            ESP_LOGI(TAG, "fc last line: %.80s", (char *)fc + (fclen > 80 ? fclen - 80 : 0));
        }
        progress("store", ESP_OK);
        err = ml_cert_store(fullchain, fclen, leaf_pem, key_pem_len);
        /* Limpiar material sensible de la copia local. */
        memset(leaf_pem, 0, pem_cap);
        free(leaf_pem);
        if (err != ESP_OK) {
            progress("store", err);
            set_error("ml_cert_store: %s", esp_err_to_name(err));
            goto out;
        }
    }

    /* Borrar fullchain (ya está en NVS via ml_cert_store). */
    free(fullchain);
    fullchain = NULL;

    /* 14) Marcar last_issue. */
    int64_t now = (int64_t)time(NULL);
    nvs_handle_t nvs;
    if (nvs_open(ML_ACME_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_i64(nvs, ML_ACME_NVS_LAST_ISSUE, now);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "cert obtained and stored");
    return ESP_OK;

out:
    free(fullchain);
    ESP_LOGE(TAG, "obtain_cert failed: %s", s_last_error);
    return err;
}

const char *ml_acme_last_error(void)
{
    return s_last_error;
}

int64_t ml_acme_last_issue_unix(void)
{
    int64_t v = 0;
    nvs_handle_t nvs;
    if (nvs_open(ML_ACME_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        if (nvs_get_i64(nvs, ML_ACME_NVS_LAST_ISSUE, &v) != ESP_OK) v = 0;
        nvs_close(nvs);
    }
    return v;
}
