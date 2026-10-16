














#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif






typedef esp_err_t (*ml_acme_set_dns_fn)(const char *name, const char *value,
                                        uint32_t timeout_ms);

typedef void (*ml_acme_progress_fn)(const char *stage, esp_err_t err);


esp_err_t ml_acme_set_directory(const char *url);

esp_err_t ml_acme_set_dns_publisher(ml_acme_set_dns_fn fn);
esp_err_t ml_acme_set_progress_cb(ml_acme_progress_fn cb);




esp_err_t ml_acme_init(void);








esp_err_t ml_acme_obtain_cert(const char *hostname);

const char *ml_acme_last_error(void);



int64_t ml_acme_last_issue_unix(void);

#ifdef __cplusplus
}
#endif
