#include "cocopa01_client.h"

#include <esp_log.h>
#include <esp_http_client.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

#include "sdkconfig.h"

static const char *TAG = "cocopa01";

// POST /<verb> a cocopa01 vía BSD sockets directos + O_NONBLOCK + select().
//
// ANTES usaba esp_http_client_perform() con timeout_ms=4000. Problema
// documentado en sesión 20 (main/state.c): en IDF v5.3 el SSL transport se
// inicializa siempre en esp_http_client_init() y esp_tls_plain_tcp_connect()
// reusa timeouts stale del contexto SSL, ignorando el timeout_ms → si
// cocopa01 no responde ARP/SYN (apagado, ruta muerta), el POST se colgaba
// 60-126s. Ese cuelgue ocurría DENTRO del task de Telegram (handlers /off,
// /suspend) y del scheduler/automation: el bot quedaba mudo minutos y
// encolaba todo. Con BSD sockets + select() el connect es determinista:
// 2s de timeout duro siempre.
static esp_err_t cocopa01_post(const char *verb) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        ESP_LOGE(TAG, "[%s] socket() failed errno=%d", verb, errno);
        return ESP_FAIL;
    }

    // O_NONBLOCK + select() para forzar timeout REAL (mismo approach que
    // cocopa01_tcp_alive() y state.c::cocopa01_http_get_body).
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags == -1 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) == -1) {
        close(sock);
        return ESP_FAIL;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(CONFIG_COCOPA01_HTTP_PORT);
    addr.sin_addr.s_addr = inet_addr(CONFIG_COCOPA01_IP);

    int rc = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        ESP_LOGD(TAG, "[%s] connect() immediate fail errno=%d", verb, errno);
        close(sock);
        return ESP_FAIL;
    }
    if (rc < 0) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
        int sr = select(sock + 1, NULL, &wfds, NULL, &tv);
        if (sr <= 0) {
            ESP_LOGD(TAG, "[%s] connect() timeout (sr=%d)", verb, sr);
            close(sock);
            return ESP_FAIL;
        }
        int sockerr = 0;
        socklen_t slen = sizeof(sockerr);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &sockerr, &slen) < 0 || sockerr != 0) {
            ESP_LOGD(TAG, "[%s] connect() failed sockerr=%d", verb, sockerr);
            close(sock);
            return ESP_FAIL;
        }
    }

    // Conectado: modo bloqueante + timeouts de send/recv por si el server
    // acepta pero no responde.
    fcntl(sock, F_SETFL, flags);
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    char req[256];
    int req_len = snprintf(req, sizeof(req),
        "POST /%s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "X-ESP32-Secret: %s\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n",
        verb, CONFIG_COCOPA01_IP, CONFIG_COCOPA01_SHUTDOWN_SECRET);
    if (req_len <= 0 || req_len >= (int)sizeof(req)) {
        close(sock);
        return ESP_FAIL;
    }

    int sent = 0;
    while (sent < req_len) {
        int n = send(sock, req + sent, req_len - sent, 0);
        if (n <= 0) {
            ESP_LOGE(TAG, "[%s] send failed: %d", verb, n);
            close(sock);
            return ESP_FAIL;
        }
        sent += n;
    }

    // Leer hasta fin de headers (\r\n\r\n) — el status code alcanza.
    char buf[512];
    int total = 0;
    int header_end = -1;
    while (total < (int)sizeof(buf) - 1) {
        int n = recv(sock, buf + total, sizeof(buf) - 1 - total, 0);
        if (n < 0) {
            ESP_LOGD(TAG, "[%s] recv error errno=%d", verb, errno);
            close(sock);
            return ESP_FAIL;
        }
        if (n == 0) break;
        total += n;
        buf[total] = 0;
        if (strstr(buf, "\r\n\r\n") != NULL) {
            header_end = total;
            break;
        }
    }
    close(sock);

    if (header_end < 0 || total == 0) {
        ESP_LOGW(TAG, "[%s] no headers received (total=%d)", verb, total);
        return ESP_FAIL;
    }

    int status_code = 0;
    if (sscanf(buf, "HTTP/1.%*d %d", &status_code) != 1 || status_code != 200) {
        ESP_LOGW(TAG, "[%s] unexpected HTTP status: %d (%.80s)", verb, status_code, buf);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "[%s] OK (HTTP %d)", verb, status_code);
    return ESP_OK;
}

esp_err_t cocopa01_send_off(void) {
    return cocopa01_post("off");
}

esp_err_t cocopa01_send_suspend(void) {
    return cocopa01_post("suspend");
}

bool cocopa01_tcp_alive(int timeout_ms) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    // O_NONBLOCK + select() para forzar timeout REAL. En lwIP v5.3 el
    // connect() bloqueante ignora SO_SNDTIMEO y puede colgarse minutos
    // retransmitiendo SYN si la IP destino no responde ARP (caso típico:
    // cocopa01 apagado). Ver detalle en main/state.c:cocopa01_http_get_body.
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags == -1 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) == -1) {
        close(sock);
        return false;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(CONFIG_COCOPA01_SSH_PORT);
    addr.sin_addr.s_addr = inet_addr(CONFIG_COCOPA01_IP);

    int rc = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        close(sock);
        return false;
    }
    if (rc < 0) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        int sr = select(sock + 1, NULL, &wfds, NULL, &tv);
        if (sr <= 0) {
            close(sock);
            return false;
        }
        int sockerr = 0;
        socklen_t slen = sizeof(sockerr);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &sockerr, &slen) < 0 || sockerr != 0) {
            close(sock);
            return false;
        }
    }
    close(sock);
    return true;
}

static esp_err_t do_get_stats(const char *host_ip, char *buf, size_t buf_size) {
    if (buf == NULL || buf_size <= 64 || host_ip == NULL || *host_ip == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = 0;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        ESP_LOGE(TAG, "[stats:%s] socket() failed errno=%d", host_ip, errno);
        return ESP_FAIL;
    }

    int flags = fcntl(sock, F_GETFL, 0);
    if (flags == -1 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) == -1) {
        close(sock);
        return ESP_FAIL;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
#ifdef CONFIG_COCOPA01_MONITOR_PORT
    addr.sin_port = htons(CONFIG_COCOPA01_MONITOR_PORT);
#else
    addr.sin_port = htons(8085);
#endif
    addr.sin_addr.s_addr = inet_addr(host_ip);

    int rc = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        ESP_LOGD(TAG, "[stats:%s] connect() immediate fail errno=%d", host_ip, errno);
        close(sock);
        return ESP_FAIL;
    }
    if (rc < 0) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
        int sr = select(sock + 1, NULL, &wfds, NULL, &tv);
        if (sr <= 0) {
            ESP_LOGD(TAG, "[stats:%s] connect() timeout (sr=%d)", host_ip, sr);
            close(sock);
            return ESP_ERR_TIMEOUT;
        }
        int sockerr = 0;
        socklen_t slen = sizeof(sockerr);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &sockerr, &slen) < 0 || sockerr != 0) {
            ESP_LOGD(TAG, "[stats:%s] connect() failed sockerr=%d", host_ip, sockerr);
            close(sock);
            return ESP_FAIL;
        }
    }

    fcntl(sock, F_SETFL, flags);
    struct timeval tv = { .tv_sec = 4, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    char req[128];
    int req_len = snprintf(req, sizeof(req),
        "GET /stats HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Connection: close\r\n"
        "User-Agent: esp32-telegram-bot/1.0\r\n"
        "\r\n",
        host_ip);
    if (req_len <= 0 || req_len >= (int)sizeof(req)) {
        close(sock);
        return ESP_FAIL;
    }

    int sent = 0;
    while (sent < req_len) {
        int n = send(sock, req + sent, req_len - sent, 0);
        if (n <= 0) {
            ESP_LOGE(TAG, "[stats:%s] send failed: %d", host_ip, n);
            close(sock);
            return ESP_FAIL;
        }
        sent += n;
    }

    int total = 0;
    while (total < (int)buf_size - 1) {
        int n = recv(sock, buf + total, buf_size - 1 - total, 0);
        if (n < 0) {
            if (total == 0) {
                close(sock);
                return ESP_FAIL;
            }
            break;
        }
        if (n == 0) break;
        total += n;
        buf[total] = 0;
    }
    close(sock);

    if (total == 0) return ESP_FAIL;

    int status_code = 0;
    if (sscanf(buf, "HTTP/1.%*d %d", &status_code) != 1 || status_code != 200) {
        ESP_LOGW(TAG, "[stats:%s] HTTP status != 200: %d", host_ip, status_code);
        return ESP_FAIL;
    }

    char *body = strstr(buf, "\r\n\r\n");
    if (body == NULL) return ESP_FAIL;
    body += 4;

    memmove(buf, body, strlen(body) + 1);
    return ESP_OK;
}

esp_err_t cocopa01_get_stats(char *buf, size_t buf_size) {
    esp_err_t err = do_get_stats(CONFIG_COCOPA01_IP, buf, buf_size);
    if (err == ESP_OK) return ESP_OK;

    if (strlen(CONFIG_COCOPA01_TS_IP) > 0 && strcmp(CONFIG_COCOPA01_IP, CONFIG_COCOPA01_TS_IP) != 0) {
        ESP_LOGW(TAG, "[stats] LAN probe to %s failed (%s), trying Tailscale IP %s",
                 CONFIG_COCOPA01_IP, esp_err_to_name(err), CONFIG_COCOPA01_TS_IP);
        err = do_get_stats(CONFIG_COCOPA01_TS_IP, buf, buf_size);
    }
    return err;
}