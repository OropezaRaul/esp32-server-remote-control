#pragma once

#include <esp_err.h>
#include <stdbool.h>

esp_err_t cocopa01_send_off(void);
esp_err_t cocopa01_send_suspend(void);
bool cocopa01_tcp_alive(int timeout_ms);
esp_err_t cocopa01_get_stats(char *buf, size_t buf_size);