
































#include "ml_peerapi.h"

#include <string.h>
#include <errno.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <esp_log.h>
#include <esp_timer.h>

static const char *TAG = "ml_peerapi";







#define PEERAPI_MAX_HEADERS     1024
#define PEERAPI_MAX_PREFIX      512
#define PEERAPI_STACK           4096
#define PEERAPI_ACCEPT_PRIO     3
#define PEERAPI_CONN_STACK      6144
#define PEERAPI_CONN_PRIO       3
#define PEERAPI_RECV_TIMEOUT_MS 5000
#define PEERAPI_TUNNEL_IDLE_S   120

#define PEERAPI_IS_CGNAT(ip) (((ip) & 0xFFC00000UL) == 0x64400000UL) 

#define FUNNEL_TLS_HOST "127.0.0.1"  
#define FUNNEL_TLS_PORT 443          

static int s_listen_fd = -1;
static uint16_t s_port = 0;
static TaskHandle_t s_task = NULL;
static volatile bool s_shutdown = false;



typedef struct {
    char method[8];
    char path[128];
    size_t content_len;      
    bool valid;
} peerapi_req_t;







static int read_request_header(int fd, peerapi_req_t *req) {
    char buf[PEERAPI_MAX_HEADERS];
    size_t got = 0;
    memset(req, 0, sizeof(*req));

    


    while (got < sizeof(buf) - 1) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) return -1;
        buf[got++] = c;
        buf[got] = '\0';

        if (got >= 4 && buf[got - 4] == '\r' && buf[got - 3] == '\n' &&
            buf[got - 2] == '\r' && buf[got - 1] == '\n') {
            break;
        }
    }

    char *sp1 = strchr(buf, ' ');
    if (!sp1) return -1;
    size_t mlen = (size_t)(sp1 - buf);
    if (mlen >= sizeof(req->method)) mlen = sizeof(req->method) - 1;
    memcpy(req->method, buf, mlen);

    char *sp2 = strchr(sp1 + 1, ' ');
    if (!sp2) return -1;
    size_t plen = (size_t)(sp2 - (sp1 + 1));
    if (plen >= sizeof(req->path)) plen = sizeof(req->path) - 1;
    memcpy(req->path, sp1 + 1, plen);
    req->valid = true;

    char *cl = NULL;
    for (char *p = buf; p < buf + got; p++) {
        if (strncasecmp(p, "content-length:", 15) == 0) { cl = p; break; }
        while (*p != '\n' && p < buf + got) p++;
    }
    if (cl) req->content_len = (size_t)strtoul(cl + 15, NULL, 10);
    return (int)got;
}

static void send_simple(int fd, int code, const char *status, const char *body) {
    char hdr[160];
    int blen = body ? (int)strlen(body) : 0;
    int hlen = snprintf(hdr, sizeof(hdr),
                        "HTTP/1.1 %d %s\r\n"
                        "Content-Type: application/json\r\n"
                        "Content-Length: %d\r\n"
                        "Connection: close\r\n"
                        "\r\n",
                        code, status, blen);
    if (hlen > 0) send(fd, hdr, (size_t)hlen, 0);
    if (blen > 0) send(fd, body, (size_t)blen, 0);
}










static void ingress_tunnel(int client_fd) {
    int up = socket(AF_INET, SOCK_STREAM, 0);
    if (up < 0) {
        ESP_LOGE(TAG, "ingress: socket failed errno=%d", errno);
        return;
    }

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = htonl(0x7F000001UL); 
    dst.sin_port = htons(FUNNEL_TLS_PORT);
    if (connect(up, (struct sockaddr *)&dst, sizeof(dst)) != 0) {
        ESP_LOGE(TAG, "ingress: connect " FUNNEL_TLS_HOST ":%u failed errno=%d",
                 (unsigned)FUNNEL_TLS_PORT, errno);
        closesocket(up);
        send_simple(client_fd, 502, "Bad Gateway", "{}");
        return;
    }

    
    static const char k101[] = "HTTP/1.1 101 Switching Protocols\r\n\r\n";
    if (send(client_fd, k101, sizeof(k101) - 1, 0) != (ssize_t)(sizeof(k101) - 1)) {
        ESP_LOGW(TAG, "ingress: failed to send 101 to relay (errno=%d)", errno);
        shutdown(client_fd, SHUT_RDWR);
        closesocket(up);
        return;
    }

    fd_set rs;
    struct timeval tv = { .tv_sec = PEERAPI_TUNNEL_IDLE_S, .tv_usec = 0 };
    uint8_t chunk[1460];
    for (;;) {
        FD_ZERO(&rs);
        FD_SET(client_fd, &rs);
        FD_SET(up, &rs);
        int ready = select(((client_fd > up) ? client_fd : up) + 1, &rs, NULL, NULL, &tv);
        if (ready <= 0) {
            if (ready == 0) {
                ESP_LOGD(TAG, "ingress: idle timeout, closing tunnel");
            }
            break;
        }
        if (FD_ISSET(client_fd, &rs)) {
            ssize_t n = recv(client_fd, chunk, sizeof(chunk), 0);
            if (n <= 0) break;
            ssize_t off = 0;
            while (off < n) {
                ssize_t w = send(up, chunk + off, (size_t)(n - off), 0);
                if (w <= 0) goto out;
                off += w;
            }
        }
        if (FD_ISSET(up, &rs)) {
            ssize_t n = recv(up, chunk, sizeof(chunk), 0);
            if (n <= 0) break;
            ssize_t off = 0;
            while (off < n) {
                ssize_t w = send(client_fd, chunk + off, (size_t)(n - off), 0);
                if (w <= 0) goto out;
                off += w;
            }
        }
    }
out:
    
    shutdown(client_fd, SHUT_RDWR);
    shutdown(up, SHUT_RDWR);
    closesocket(up);
    
}



static void peerapi_conn_task(void *arg) {
    int fd = (int)(intptr_t)arg;

    struct sockaddr_storage peer;
    socklen_t plen = sizeof(peer);
    if (getpeername(fd, (struct sockaddr *)&peer, &plen) != 0) {
        ESP_LOGD(TAG, "getpeername failed, dropping");
        goto out;
    }

    char src_str[64] = {0};
    bool allowed = false;

    if (peer.ss_family == AF_INET) {
        struct sockaddr_in *peer4 = (struct sockaddr_in *)&peer;
        uint32_t ip4 = ntohl(peer4->sin_addr.s_addr);
        inet_ntop(AF_INET, &peer4->sin_addr, src_str, sizeof(src_str));
        if (PEERAPI_IS_CGNAT(ip4)) {
            allowed = true;
        }
    }
#if LWIP_IPV6
    else if (peer.ss_family == AF_INET6) {
        struct sockaddr_in6 *peer6 = (struct sockaddr_in6 *)&peer;
        inet_ntop(AF_INET6, &peer6->sin6_addr, src_str, sizeof(src_str));
        const uint8_t *b = (const uint8_t *)&peer6->sin6_addr.s6_addr;
        
        bool v4_mapped = true;
        for (int i = 0; i < 10; i++) {
            if (b[i] != 0) { v4_mapped = false; break; }
        }
        if (v4_mapped && b[10] == 0xff && b[11] == 0xff) {
            uint32_t ip4 = ((uint32_t)b[12] << 24) | ((uint32_t)b[13] << 16) |
                           ((uint32_t)b[14] << 8) | (uint32_t)b[15];
            if (PEERAPI_IS_CGNAT(ip4)) {
                allowed = true;
            }
        } else {
            
            if (b[0] == 0xfd && b[1] == 0x7a && b[2] == 0x11 &&
                b[3] == 0x5c && b[4] == 0xa1 && b[5] == 0xe0) {
                allowed = true;
            }
        }
    }
#endif

    if (!allowed) {
        ESP_LOGW(TAG, "conn from non-tailnet source (%s), dropping", src_str);
        goto out;
    }

    {
        peerapi_req_t req;
        int hlen = read_request_header(fd, &req);
        if (hlen < 0 || !req.valid) {
            send_simple(fd, 400, "Bad Request", "{}");
            goto out;
        }

        ESP_LOGI(TAG, "peerapi: %s %s (len=%u) src=%s", req.method, req.path,
                 (unsigned)req.content_len, src_str);

        if (strcmp(req.path, "/v0/ingress") == 0 && strcmp(req.method, "POST") == 0) {
            ESP_LOGI(TAG, "ingress: funnel tunnel opened (clean stream)");
            ingress_tunnel(fd);
            ESP_LOGI(TAG, "ingress: funnel tunnel closed");
            goto out;  
        }

        if (strcmp(req.method, "POST") == 0) {
            send_simple(fd, 200, "OK", "{}");
        } else {
            send_simple(fd, 200, "OK", "{\"status\":\"microlink-peerapi\"}");
        }
    }

out:
    

    ESP_LOGI(TAG, "peerapi: conn done, stack high-water %uB free",
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
    shutdown(fd, SHUT_RDWR);
    closesocket(fd);
    vTaskDelete(NULL);
}



static void peerapi_accept_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "PeerAPI listener started on port %u (IPv4+IPv6 dual-stack ready)",
             (unsigned)s_port);

    while (!s_shutdown) {
        struct sockaddr_storage peer;
        socklen_t plen = sizeof(peer);
        int fd = accept(s_listen_fd, (struct sockaddr *)&peer, &plen);
        if (fd < 0) {
            if (s_shutdown) break;
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                ESP_LOGW(TAG, "accept failed: errno=%d", errno);
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            continue;
        }

        struct timeval tv = { .tv_sec = PEERAPI_RECV_TIMEOUT_MS / 1000, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (xTaskCreate(peerapi_conn_task, "ml_peerapi_c", PEERAPI_CONN_STACK,
                        (void *)(intptr_t)fd, PEERAPI_CONN_PRIO, NULL) != pdPASS) {
            closesocket(fd);
        }
    }

    ESP_LOGI(TAG, "PeerAPI listener stopped");
    vTaskDelete(NULL);
}



esp_err_t ml_peerapi_start(uint16_t port) {
    if (port == 0) return ESP_ERR_INVALID_ARG;
    if (s_task) return ESP_ERR_INVALID_STATE;

#if LWIP_IPV6
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) {
        ESP_LOGE(TAG, "socket AF_INET6 failed: errno=%d, falling back to AF_INET", errno);
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            ESP_LOGE(TAG, "socket AF_INET failed: errno=%d", errno);
            return ESP_FAIL;
        }
    }
#else
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        ESP_LOGE(TAG, "socket failed: errno=%d", errno);
        return ESP_FAIL;
    }
#endif

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

#if LWIP_IPV6
    
    int zero = 0;
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));

    struct sockaddr_in6 addr6 = {0};
    addr6.sin6_family = AF_INET6;
    addr6.sin6_addr = in6addr_any;
    addr6.sin6_port = htons(port);
    if (bind(fd, (struct sockaddr *)&addr6, sizeof(addr6)) != 0) {
        ESP_LOGE(TAG, "bind IPv6 :%u failed: errno=%d", (unsigned)port, errno);
        close(fd);
        return ESP_FAIL;
    }
#else
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind :%u failed: errno=%d", (unsigned)port, errno);
        close(fd);
        return ESP_FAIL;
    }
#endif
    if (listen(fd, 4) != 0) {
        ESP_LOGE(TAG, "listen :%u failed: errno=%d", (unsigned)port, errno);
        close(fd);
        return ESP_FAIL;
    }

    s_listen_fd = fd;
    s_port = port;
    s_shutdown = false;

    if (xTaskCreate(peerapi_accept_task, "ml_peerapi", PEERAPI_STACK,
                    NULL, PEERAPI_ACCEPT_PRIO, &s_task) != pdPASS) {
        close(fd);
        s_listen_fd = -1;
        s_port = 0;
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t ml_peerapi_stop(void) {
    if (!s_task) return ESP_OK;
    s_shutdown = true;
    if (s_listen_fd >= 0) {
        shutdown(s_listen_fd, SHUT_RDWR);
        close(s_listen_fd);
        s_listen_fd = -1;
    }
    
    vTaskDelay(pdMS_TO_TICKS(200));
    s_task = NULL;
    s_port = 0;
    return ESP_OK;
}

uint16_t ml_peerapi_get_port(void) {
    return s_port;
}
