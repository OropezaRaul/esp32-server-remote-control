#pragma once

#include <esp_err.h>
#include "sdkconfig.h"

esp_err_t web_server_start(void);
esp_err_t web_server_stop(void);


esp_err_t web_server_funnel_start(void);
esp_err_t web_server_funnel_stop(void);

#if CONFIG_ESP_HTTPS_SERVER_ENABLE





esp_err_t web_server_funnel_tls_start(void);
esp_err_t web_server_funnel_tls_stop(void);
esp_err_t web_server_funnel_tls_reload(void);
const char *web_server_funnel_tls_cert_source(void);
#endif