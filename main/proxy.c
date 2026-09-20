#include "proxy.h"

#include <esp_log.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <sys/select.h>
#include <errno.h>

#include "automation.h"
#include "wol.h"
#include "cocopa01_client.h"
#include "state.h"
#include "microlink_wrapper.h"

#include "sdkconfig.h"

static const char *TAG = "proxy";

// ─── HTTP detection & redirect ────────────────────────────────
static bool looks_like_http(const char *buf, int len) {
    static const char *methods[] = {
        "GET ", "POST ", "PUT ", "DELETE ", "HEAD ",
        "OPTIONS ", "PATCH ", "CONNECT ", "TRACE "
    };
    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
        size_t mlen = strlen(methods[i]);
        if (len >= (int)mlen && memcmp(buf, methods[i], mlen) == 0) return true;
    }
    return false;
}

static int parse_http_path(const char *buf, int len, char *out, int out_size) {
    int i = 0;
    while (i < len && buf[i] != ' ') i++;
    if (i >= len) return -1;
    i++;
    int j = 0;
    while (i < len && buf[i] != ' ' && buf[i] != '\r' && buf[i] != '\n'
           && j < out_size - 1) {
        out[j++] = buf[i++];
    }
    out[j] = 0;
    return j;
}

// Decide qué IP de cocopa01 usar cuando el mapping tiene upstream_host=""
// (auto-routing). Peer en LAN -> cocopa01 LAN, peer por Tailscale -> cocopa01 TS.
static const char *auto_route_cocopa01_host(struct sockaddr_in *peer) {
    uint32_t a = ntohl(peer->sin_addr.s_addr);
    if ((a & 0xFFFF0000UL) == 0xC0A80000UL) return CONFIG_COCOPA01_IP;
    return CONFIG_COCOPA01_TS_IP;
}

// Resuelve el upstream_host efectivo para esta conexión: lo declarado en
// el entry, o auto-routing si está vacío. SIEMPRE copia el resultado a
// `scratch` para que el caller lo use sin生命周期 worries.
static const char *resolve_host(const portmap_entry_t *entry,
                                struct sockaddr_in *peer,
                                char *scratch, size_t scratch_size) {
    const char *src;
    if (entry->upstream_host[0]) {
        src = entry->upstream_host;
    } else {
        src = auto_route_cocopa01_host(peer);
    }
    strncpy(scratch, src, scratch_size - 1);
    scratch[scratch_size - 1] = 0;
    return scratch;
}

// ─── Upstream connect ─────────────────────────────────────────
// Acepta IP literal (inet_aton) o hostname (gethostbyname). DNS lookup puede
// demorar ~100ms-2s si el resolver está frío.
static int upstream_connect(const char *host, uint16_t port, int timeout_ms) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        ESP_LOGE(TAG, "upstream_connect: socket() failed errno=%d", errno);
        return -1;
    }

    struct timeval tv = {
        .tv_sec  = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    // Bind to VPN IP so lwIP routes through WireGuard netif for Tailscale targets
    uint32_t vpn_ip = microlink_wrapper_get_vpn_ip();
    if (vpn_ip != 0) {
        struct sockaddr_in src = {
            .sin_family = AF_INET,
            .sin_port   = 0,
            .sin_addr.s_addr = htonl(vpn_ip),
        };
        bind(sock, (struct sockaddr *)&src, sizeof(src));
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    addr.sin_addr.s_addr = inet_addr(host);
    if (addr.sin_addr.s_addr == INADDR_NONE) {
        // inet_aton falló (no es IP literal) -> DNS lookup
        struct hostent *he = gethostbyname(host);
        if (he == NULL || he->h_addr_list[0] == NULL) {
            ESP_LOGE(TAG, "upstream_connect: DNS lookup failed for %s", host);
            close(sock);
            return -1;
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(addr.sin_addr));
    }

    int rc = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (rc != 0) {
        ESP_LOGW(TAG, "upstream_connect: connect(%s:%u) failed errno=%d (%s) tunnel=%s",
                 host, (unsigned)port, errno, strerror(errno),
                 microlink_wrapper_peer_is_up(ntohl(addr.sin_addr.s_addr)) ? "UP" : "DOWN");
        close(sock);
        return -1;
    }
    return sock;
}

// Probe rápido (no bloquea más de `timeout_ms` por intento). Retorna true si
// el destino responde.
static bool upstream_probe(const char *host, uint16_t port, int timeout_ms) {
    int s = upstream_connect(host, port, timeout_ms);
    if (s < 0) return false;
    close(s);
    return true;
}

// ─── HTTP redirect ────────────────────────────────────────────
static int send_http_redirect(int client_sock, const char *proto,
                              const char *host, uint16_t port,
                              const char *path) {
    char resp[1024];
    int len = snprintf(resp, sizeof(resp),
        "HTTP/1.1 302 Found\r\n"
        "Location: %s://%s:%u%s%s\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n",
        proto, host, (unsigned)port,
        (path[0] == '/') ? "" : "/", path);
    if (len <= 0 || len >= (int)sizeof(resp)) return -1;
    int w = send(client_sock, resp, len, 0);
    ESP_LOGI(TAG, "redirect sent (%d bytes) -> %s://%s:%u%s%s",
             w, proto, host, (unsigned)port,
             (path[0] == '/') ? "" : "/", path);
    return w;
}

// ─── Forward loop (TCP crudo bidireccional) ───────────────────
static void forward_loop(int client_sock, int upstream_sock) {
    char buf[4096];
    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(client_sock, &rfds);
        FD_SET(upstream_sock, &rfds);
        int maxfd = (client_sock > upstream_sock) ? client_sock : upstream_sock;

        struct timeval tv = {.tv_sec = PROXY_FWD_TIMEOUT_S, .tv_usec = 0};
        int ret = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) { ESP_LOGW(TAG, "fwd sel err"); break; }
        if (ret == 0) { ESP_LOGI(TAG, "fwd idle"); break; }
        if (FD_ISSET(client_sock, &rfds)) {
            int n = recv(client_sock, buf, sizeof(buf), 0);
            if (n <= 0) break;
            int off = 0;
            while (off < n) {
                int w = send(upstream_sock, buf + off, n - off, 0);
                if (w <= 0) { close(client_sock); close(upstream_sock); return; }
                off += w;
            }
        }
        if (FD_ISSET(upstream_sock, &rfds)) {
            int n = recv(upstream_sock, buf, sizeof(buf), 0);
            if (n <= 0) break;
            int off = 0;
            while (off < n) {
                int w = send(client_sock, buf + off, n - off, 0);
                if (w <= 0) { close(client_sock); close(upstream_sock); return; }
                off += w;
            }
        }
    }
}

// ─── Wake-on-demand ───────────────────────────────────────────
// Garantiza que el destino esté respondiendo. Si está caído, manda WOL y
// espera (con reintentos) hasta `timeout_ms` o que responda. Devuelve true
// si respondió dentro del timeout.
static bool ensure_upstream_up(const char *host, uint16_t port, uint32_t timeout_ms) {
    // First probe gets a longer timeout — covers the case where the WG tunnel
    // is not yet up and needs to handshake first (which takes ~1-3s).
    if (upstream_probe(host, port, 3000)) {
        ESP_LOGI(TAG, "ensure: %s:%u already up", host, (unsigned)port);
        return true;
    }

    cocopa01_state_t st = state_get();
    ESP_LOGI(TAG, "ensure: %s:%u down (state=%s), sending WOL",
             host, (unsigned)port, cocopa01_state_str(st));
    esp_err_t wres = wol_send_default();
    if (wres != ESP_OK) {
        ESP_LOGW(TAG, "ensure: WOL failed: %s", esp_err_to_name(wres));
        // No retornamos false todavía — puede que ya esté booteando.
    }

    uint32_t start_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    int attempts = 0;
    while (1) {
        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (now_ms - start_ms >= timeout_ms) break;
        vTaskDelay(pdMS_TO_TICKS(PROXY_WAKE_POLL_MS));
        attempts++;
        if (upstream_probe(host, port, 3000)) {
            uint32_t elapsed = now_ms - start_ms;
            ESP_LOGI(TAG, "ensure: %s:%u up after %u ms (%d probes)",
                     host, (unsigned)port, (unsigned)elapsed, attempts);
            state_mark_known_on();
            return true;
        }
    }
    ESP_LOGW(TAG, "ensure: %s:%u didn't respond in %u ms",
             host, (unsigned)port, (unsigned)timeout_ms);
    return false;
}

// ─── Per-connection handler ───────────────────────────────────
typedef struct {
    int                client_sock;
    struct sockaddr_in peer;
    portmap_entry_t    entry;  // copia por valor — sobrevive a portmap_del
    char               host[PORTMAP_HOST_MAX];  // host resuelto (auto o explícito)
} proxy_conn_arg_t;

static void handle_connection(void *arg) {
    proxy_conn_arg_t *carg = (proxy_conn_arg_t *)arg;

    char peer_ip[16] = {0};
    inet_ntoa_r(carg->peer.sin_addr, peer_ip, sizeof(peer_ip));
    ESP_LOGI(TAG, "conn from %s:%u on :%u -> %s://%s:%u [%s]",
             peer_ip, (unsigned)ntohs(carg->peer.sin_port),
             (unsigned)carg->entry.local_port,
             carg->entry.proto, carg->host, (unsigned)carg->entry.upstream_port,
             carg->entry.label);

    // Actividad de negocio: kick.
    automation_kick();

    struct timeval tv = {.tv_sec = PROXY_HTTP_PROBE_MS / 1000,
                         .tv_usec = (PROXY_HTTP_PROBE_MS % 1000) * 1000};
    setsockopt(carg->client_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(carg->client_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    char buf[1024];
    int n = recv(carg->client_sock, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        ESP_LOGW(TAG, "recv err n=%d errno=%d", n, errno);
        close(carg->client_sock);
        free(carg);
        vTaskDelete(NULL);
        return;
    }
    buf[n] = 0;

    if (looks_like_http(buf, n)) {
        char path[1024] = "/";
        if (parse_http_path(buf, n, path, sizeof(path)) < 0) {
            path[0] = '/'; path[1] = 0;
        }
        // Garantizar upstream arriba antes del redirect.
        ensure_upstream_up(carg->host, carg->entry.upstream_port,
                           PROXY_WAKE_TIMEOUT_MS);
        send_http_redirect(carg->client_sock, carg->entry.proto,
                           carg->host, carg->entry.upstream_port, path);
        close(carg->client_sock);
        free(carg);
        vTaskDelete(NULL);
        return;
    }

    // No-HTTP (SSH/lo que sea): TCP forward.
    if (!ensure_upstream_up(carg->host, carg->entry.upstream_port,
                            PROXY_WAKE_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "upstream %s:%u unreachable, dropping tcp conn",
                 carg->host, (unsigned)carg->entry.upstream_port);
        close(carg->client_sock);
        free(carg);
        vTaskDelete(NULL);
        return;
    }
    state_mark_known_on();
    int upstream = upstream_connect(carg->host, carg->entry.upstream_port, 2000);
    if (upstream < 0) {
        close(carg->client_sock);
        free(carg);
        vTaskDelete(NULL);
        return;
    }
    if (send(upstream, buf, n, 0) < 0) {
        close(carg->client_sock); close(upstream);
        free(carg);
        vTaskDelete(NULL);
        return;
    }
    forward_loop(carg->client_sock, upstream);
    close(carg->client_sock); close(upstream);
    free(carg);
    vTaskDelete(NULL);
}

// ─── Listener task ────────────────────────────────────────────
static void proxy_listener_task(void *arg) {
    proxy_listener_ctx_t *ctx = (proxy_listener_ctx_t *)arg;
    ESP_LOGI(TAG, "listener up on :%u (label=%s)",
             (unsigned)ctx->entry.local_port, ctx->entry.label);

    while (!ctx->stopping) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int client_sock = accept(ctx->listen_sock, (struct sockaddr *)&peer, &plen);
        if (client_sock < 0) {
            if (errno == EBADF || errno == EINVAL) break;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;  // timeout, re-check stopping
            ESP_LOGW(TAG, "accept err=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        proxy_conn_arg_t *carg = malloc(sizeof(proxy_conn_arg_t));
        if (!carg) {
            ESP_LOGW(TAG, "OOM allocating conn arg; dropping");
            close(client_sock);
            continue;
        }
        carg->client_sock = client_sock;
        carg->peer        = peer;
        carg->entry       = ctx->entry;

        // Resolver host efectivo para ESTA conexión (puede diferir si es
        // auto-routing y el peer cambió de LAN a TS o viceversa).
        const char *resolved = resolve_host(&carg->entry, &peer,
                                            carg->host, sizeof(carg->host));
        // Use resolved (ya está copiado a carg->host) por consistencia.
        (void)resolved;

        BaseType_t ok = xTaskCreate(handle_connection, "proxy_conn",
                                    PROXY_TASK_STACK, carg,
                                    PROXY_TASK_PRIO, NULL);
        if (ok != pdPASS) {
            ESP_LOGW(TAG, "xTaskCreate failed; dropping");
            close(client_sock);
            free(carg);
        }
    }

    ESP_LOGI(TAG, "listener exiting (port %u, %s)",
             (unsigned)ctx->entry.local_port,
             ctx->stopping ? "stop requested" : "socket error");
    ctx->task = NULL;  // señaliza a proxy_listener_destroy que puede liberar
    vTaskDelete(NULL);
}

// ─── Public API ───────────────────────────────────────────────
proxy_listener_ctx_t *proxy_listener_create(const portmap_entry_t *entry) {
    proxy_listener_ctx_t *ctx = calloc(1, sizeof(proxy_listener_ctx_t));
    if (!ctx) return NULL;
    ctx->entry       = *entry;
    ctx->listen_sock = -1;
    ctx->task        = NULL;

    ctx->listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (ctx->listen_sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
        free(ctx);
        return NULL;
    }
    int opt = 1;
    setsockopt(ctx->listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(entry->local_port);

    if (bind(ctx->listen_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind :%u failed: errno=%d (port in use?)",
                 (unsigned)entry->local_port, errno);
        close(ctx->listen_sock);
        free(ctx);
        return NULL;
    }
    if (listen(ctx->listen_sock, PROXY_LISTEN_BACKLOG) != 0) {
        ESP_LOGE(TAG, "listen :%u failed: errno=%d",
                 (unsigned)entry->local_port, errno);
        close(ctx->listen_sock);
        free(ctx);
        return NULL;
    }

    // Set accept() timeout so the listener task can check the stopping flag
    // periodically without relying on select() (which may have issues with
    // the WireGuard netif in lwIP).
    struct timeval accept_tv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(ctx->listen_sock, SOL_SOCKET, SO_RCVTIMEO, &accept_tv, sizeof(accept_tv));

    BaseType_t ok = xTaskCreate(proxy_listener_task, "proxy_lis",
                                PROXY_LIS_TASK_STACK, ctx,
                                PROXY_LIS_TASK_PRIO, &ctx->task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(listener :%u) failed",
                 (unsigned)entry->local_port);
        close(ctx->listen_sock);
        free(ctx);
        return NULL;
    }

    return ctx;
}

void proxy_listener_destroy(proxy_listener_ctx_t *ctx) {
    if (!ctx) return;
    ESP_LOGI(TAG, "destroying listener on :%u",
             (unsigned)ctx->entry.local_port);
    ctx->stopping = true;
    if (ctx->listen_sock >= 0) {
        close(ctx->listen_sock);
        ctx->listen_sock = -1;
    }
    // Esperar hasta 2s a que la task se autodestruya.
    for (int i = 0; i < 20; i++) {
        if (ctx->task == NULL) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    free(ctx);
}
