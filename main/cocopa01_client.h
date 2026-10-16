#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

esp_err_t cocopa01_send_off(void);
esp_err_t cocopa01_send_suspend(void);
bool cocopa01_tcp_port_alive(uint16_t port, int timeout_ms);
bool cocopa01_tcp_alive(int timeout_ms);
bool cocopa01_web_alive(int timeout_ms);
esp_err_t cocopa01_get_stats(char *buf, size_t buf_size);



bool cocopa01_tcp_port_alive_at(const char *ip, uint16_t port, int timeout_ms);













int cocopa01_http_probe_at(const char *ip, uint16_t port, const char *host_header,
                           const char *path, int timeout_ms);
