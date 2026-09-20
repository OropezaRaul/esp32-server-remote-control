#pragma once

#include <esp_err.h>
#include "sdkconfig.h"

esp_err_t web_server_start(void);
esp_err_t web_server_stop(void);

/* Tailscale Funnel */
esp_err_t web_server_funnel_start(void);
esp_err_t web_server_funnel_stop(void);

#if CONFIG_ESP_HTTPS_SERVER_ENABLE
/* Tailscale Funnel con TLS terminado en el ESP32.
 * El cert se carga de NVS primero (emitido por ACME client), fallback al
 * cert autofirmado hardcoded.
 * tls_reload() recarga con el cert que esté en NVS ahora (sin reiniciar ESP32).
 * tls_cert_source() devuelve "nvs" o "fallback-hardcoded" para diagnóstico. */
esp_err_t web_server_funnel_tls_start(void);
esp_err_t web_server_funnel_tls_stop(void);
esp_err_t web_server_funnel_tls_reload(void);
const char *web_server_funnel_tls_cert_source(void);
#endif