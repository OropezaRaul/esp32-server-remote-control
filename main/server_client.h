#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

esp_err_t server_send_off(void);
esp_err_t server_send_suspend(void);
bool server_tcp_port_alive(uint16_t port, int timeout_ms);
bool server_tcp_alive(int timeout_ms);
// El daemon nativo (:9999) responde? Es el canal que puede ejecutar el
// apagado; se consulta aparte de :22 porque los dos channels se apagan en
// momentos distintos (el daemon primero, sshd despues) y porque :22 puede
// estar caido mientras el daemon sigue vivo.
bool server_daemon_alive(int timeout_ms);
bool server_web_alive(int timeout_ms);
esp_err_t server_get_stats(char *buf, size_t buf_size);





bool server_state_alive_now(int timeout_ms);



bool server_tcp_port_alive_at(const char *ip, uint16_t port, int timeout_ms);













int server_http_probe_at(const char *ip, uint16_t port, const char *host_header,
                           const char *path, int timeout_ms);
