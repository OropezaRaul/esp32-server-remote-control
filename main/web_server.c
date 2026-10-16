#include "web_server.h"

#include <esp_log.h>
#include <esp_http_server.h>
#if CONFIG_ESP_HTTPS_SERVER_ENABLE
#include <esp_https_server.h>
#include <mbedtls/x509_crt.h>
#include "ml_cert.h"
#endif
#include <esp_netif.h>
#include <cJSON.h>
#include <string.h>
#include <stdlib.h>
#include <lwip/sockets.h>

#include "ready_probe.h"
#include "state.h"
#include "wake.h"
#include "wol.h"
#include "server_client.h"
#include "automation.h"
#include "scheduler.h"
#include "telegram.h"
#include "microlink_wrapper.h"
#include "microlink_watchdog.h"
#include "portmap.h"
#include "version.h"

#include "sdkconfig.h"

#if CONFIG_ESP_HTTPS_SERVER_ENABLE
#include "test_certs/test_certs.h"
#endif

static const char *TAG = "web";

static httpd_handle_t s_server = NULL;
static httpd_handle_t s_server_funnel = NULL;

// TECHO DE SEGURIDAD, ya no el camino normal.
//
// Antes este valor era la decision completa: tras ver server ON el portal
// esperaba 45 s fijos antes de redirigir, y hasta 75 s si no habia podido leer
// /state en vivo. Medido en campo: el Funnel de server ya servia 200 a los ~46 s
// del WOL y el navegador no entraba hasta los ~78 s. Esos ~25 s eran el reloj
// esperando a ciegas, y un reloj no sabe nada del arranque real.
//
// Ahora la decision la toma la EVIDENCIA (main/ready_probe.c): una sonda que el
// ESP32 le hace al propio destino, o el aviso que server manda con
// GET /notify/ready en cuanto se verifica a si mismo. Este techo solo se consulta
// cuando no hay ninguna medicion disponible (sin tailnet, TLS roto, :9999 caido),
// para que el portal nunca se quede girando para siempre. Ver server_target_ready.
#define SERVER_STACK_SETTLE_MS 45000UL

static void send_text(httpd_req_t *req, const char *status, const char *body) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_send(req, body, strlen(body));
}

static void send_json(httpd_req_t *req, const char *body) {
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, strlen(body));
}

static esp_err_t get_query_param(httpd_req_t *req, const char *name,
                                  char *out, size_t out_size) {
    if (req == NULL || name == NULL || out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0) return ESP_ERR_NOT_FOUND;
    char *qbuf = malloc(qlen + 1);
    if (qbuf == NULL) return ESP_ERR_NO_MEM;
    if (httpd_req_get_url_query_str(req, qbuf, qlen + 1) != ESP_OK) {
        free(qbuf);
        return ESP_FAIL;
    }
    esp_err_t err = httpd_query_key_value(qbuf, name, out, out_size);
    free(qbuf);
    return err;
}

static esp_err_t handle_wol(httpd_req_t *req) {
    ESP_LOGI(TAG, "GET /wol from client");
    automation_kick();

    char mac_str[32] = {0};
    bool has_mac = (get_query_param(req, "mac", mac_str, sizeof(mac_str)) == ESP_OK);

    if (has_mac) {
        // /wol?mac=XX:XX... es una herramienta de diagnostico con MAC
        // arbitraria: no entra al supervisor (que siempre apunta a
        // CONFIG_SERVER_MAC_HEX), solo manda la rafaga puntual.
        ESP_LOGI(TAG, "  mac=%s (rafaga puntual, sin supervisor)", mac_str);
        esp_err_t err = wol_send_str(mac_str);
        if (err != ESP_OK) {
            send_text(req, "500 Internal Server Error", "wol_send failed\n");
            return ESP_FAIL;
        }
        state_mark_known_on();
        send_text(req, "202 Accepted", "magic packet sent\n");
        return ESP_OK;
    }

    // Camino normal: el supervisor manda la rafaga YA y reintenta solo cada
    // 10 s hasta que server responda (maximo 4 min). Antes este handler
    // mandaba una unica rafaga y contestaba "retry in 30-60s" sin reintentar
    // nada: si esa rafaga se perdia, nadie volvia a intentarlo.
    esp_err_t err = wake_request("http /wol");
    if (err != ESP_OK) {
        send_text(req, "500 Internal Server Error", "wol_send failed\n");
        return ESP_FAIL;
    }
    state_mark_known_on();

    char body[160];
    snprintf(body, sizeof(body),
             "waking up -- reintentando WOL cada %us hasta %u min o hasta que "
             "server responda. Estado: /diag\n",
             (unsigned)(WAKE_RETRY_MS / 1000), (unsigned)(WAKE_WINDOW_MS / 60000));
    send_text(req, "202 Accepted", body);
    return ESP_OK;
}

static esp_err_t handle_status(httpd_req_t *req) {
    ESP_LOGI(TAG, "GET /status");
    automation_kick();
    server_state_t st = state_get();
    uint32_t in_ms = state_entered_at_ms();
    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    uint32_t elapsed = (in_ms > 0) ? (now_ms - in_ms) : 0;
    uint32_t boot_time = state_server_boot_time();
    uint32_t uptime_s = state_server_uptime_s();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state", server_state_str(st));
    cJSON_AddNumberToObject(root, "state_age_ms", elapsed);
    cJSON_AddNumberToObject(root, "server_boot_time", boot_time);
    cJSON_AddNumberToObject(root, "server_uptime_s", uptime_s);
    cJSON_AddStringToObject(root, "server_ip", CONFIG_SERVER_IP);
    cJSON_AddStringToObject(root, "server_mac", CONFIG_SERVER_MAC_HEX);
    cJSON_AddStringToObject(root, "wake", wake_status_text());
    cJSON_AddNumberToObject(root, "heap_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "psram_free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    send_json(req, body);
    free(body);
    return ESP_OK;
}

static esp_err_t handle_portmap(httpd_req_t *req) {
    ESP_LOGI(TAG, "GET /portmap");
    automation_kick();

    int count = 0;
    portmap_entry_t entries[PORTMAP_MAX_ENTRIES];
    esp_err_t err = portmap_get_all(entries, PORTMAP_MAX_ENTRIES, &count);
    if (err != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "count", count);
    cJSON_AddNumberToObject(root, "max", PORTMAP_MAX_ENTRIES);
    cJSON *arr = cJSON_AddArrayToObject(root, "entries");
    for (int i = 0; i < count; i++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "local_port", entries[i].local_port);
        cJSON_AddStringToObject(obj, "upstream_host", entries[i].upstream_host);
        cJSON_AddNumberToObject(obj, "upstream_port", entries[i].upstream_port);
        cJSON_AddStringToObject(obj, "proto", entries[i].proto);
        cJSON_AddStringToObject(obj, "label", entries[i].label);
        cJSON_AddItemToArray(arr, obj);
    }
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    send_json(req, body);
    free(body);
    return ESP_OK;
}

static esp_err_t handle_root(httpd_req_t *req) {
    automation_kick();
    const char *html =
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>ESP32 Remote Control</title>"
        "<style>"
        "body{font-family:system-ui,-apple-system,sans-serif;background:#121212;color:#eee;margin:0;padding:24px;display:flex;flex-direction:column;align-items:center;}"
        "h1{color:#4ade80;margin-bottom:8px;font-size:24px;}p{color:#aaa;margin-top:0;font-size:14px;}"
        ".grid{display:flex;flex-direction:column;gap:12px;width:100%;max-width:380px;margin-top:16px;}"
        ".btn{padding:14px 18px;background:#2563eb;color:#fff;text-decoration:none;border-radius:12px;text-align:center;font-weight:600;font-size:15px;box-shadow:0 4px 12px rgba(0,0,0,0.3);}"
        ".btn:hover{background:#1d4ed8;}"
        ".btn-primary{background:#059669;}"
        ".btn-primary:hover{background:#047857;}"
        ".btn-sec{background:#27272a;color:#d4d4d8;}"
        ".btn-sec:hover{background:#3f3f46;}"
        "</style></head><body>"
        "<h1>ESP32 Remote Control</h1>"
        "<p>Entrada pública Tailscale con Auto-WOL</p>"
        "<div class='grid'>"
        "<a class='btn btn-primary' href='/app'>🍔 Entrar a the web app (Auto-WOL)</a>"
        "<a class='btn' href='/monitor'>📊 Monitor (:8085)</a>"
        "<a class='btn btn-sec' href='/kiosk'>✨ Entrar a Kiubart (Auto-WOL)</a>"
        "<a class='btn btn-sec' href='/wol'>⚡ Enviar WOL ahora</a>"
        "<a class='btn btn-sec' href='/status'>ℹ️ Estado del Sistema (/status)</a>"
        "</div></body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, html, strlen(html));
    return ESP_OK;
}

static esp_err_t handle_test_tg(httpd_req_t *req) {
    ESP_LOGI(TAG, "GET /test_tg");
    esp_err_t err = telegram_broadcast("Test from ESP32 web /test_tg");
    if (err == ESP_OK) {
        send_text(req, "200 OK", "telegram broadcast sent\n");
    } else {
        send_text(req, "500 Internal Server Error", "telegram broadcast failed\n");
    }
    return ESP_OK;
}

// ─── /gate: wake-on-demand + redirect (works on port 80 via Tailscale) ──

static const char *gate_peer_ip(httpd_req_t *req) {
    int sockfd = httpd_req_to_sockfd(req);
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    if (getpeername(sockfd, (struct sockaddr *)&peer, &plen) == 0) {
        static char ip_str[16];
        inet_ntoa_r(peer.sin_addr, ip_str, sizeof(ip_str));
        return ip_str;
    }
    return NULL;
}

static bool gate_is_lan_peer(const char *ip_str) {
    if (!ip_str) return false;
    // Check 192.168.x.x
    return strncmp(ip_str, "192.168.", 8) == 0;
}






























static const char *s_ready_source = "ninguna";

static bool server_target_ready(void) {
    // server dijo que SU ruta publica no sirve: manda por encima de todo lo que
    // el ESP32 puede ver solo (el tunel cae aunque la LAN siga sirviendo, y ahi
    // nuestra sonda daria listo y el cliente se comeria un 502 sin cura).
    if (ready_public_broken()) { s_ready_source = "servidor:roto"; return false; }
    if (ready_is_ready()) { s_ready_source = "sonda"; return true; }
    if (state_stack_never_stopped()) { s_ready_source = "resume-s3"; return true; }
    if (state_stack_settled(SERVER_STACK_SETTLE_MS)) { s_ready_source = "techo"; return true; }
    s_ready_source = "ninguna";
    return false;
}

static bool server_verified_alive(server_state_t st, bool cached_tcp) {
    (void)cached_tcp;
    if (st != SERVER_STATE_ON) return false;
    // El servidor dijo que su ruta publica no sirve: ni sonda ni heuristica pueden
    // abrir el gate contra su palabra.
    if (ready_public_broken()) return false;
    // Evidencia directa y reciente (sonda con 200 real, o aviso de server). Es
    // MAS fuerte que un connect() a :22: la sonda ya vio al backend contestando, y
    // con la maquina recien encendida y saturada por Docker un connect() de 250 ms
    // puede dar falso negativo y costingnos un viaje extra del cliente.
    if (ready_is_ready()) return true;
    // Sin evidencia directa (resume desde S3, o el techo de seguridad) si exigimos
    // un connect() real en el instante del 302: ahi la maquina esta estable y
    // cuesta 1-3 ms, y es la unica red contra un 302 a un servidor caido.
    if (state_stack_never_stopped() || state_stack_settled(SERVER_STACK_SETTLE_MS)) {
        return server_tcp_alive(250);
    }
    return false;
}





static bool server_ready_cached(void) {
    if (state_get() != SERVER_STATE_ON) return false;
    // Con latch fresco no exigimos nada mas: el /state en vivo puede tardar 30 s en
    // llegar cuando la maquina viene de un arranque, y hacer esperar al splash por
    // el poll seria volver aicronizar con el reloj que acabamos de eliminar.
    if (ready_is_ready()) return true;
    return server_target_ready() && state_last_poll_tcp_alive();
}

static esp_err_t send_wake_splash(httpd_req_t *req, const char *target_url, const char *title) {
    // Hay alguien esperando: el sondeo de readiness pasa a 1 s mientras dure la
    // ventana (READY_LOGIC_ARM_MS), y se renueva en cada recarga del splash.
    ready_arm();
    char *buf = malloc(2500);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    int len = snprintf(buf, 2500,
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<noscript><meta http-equiv='refresh' content='8'></noscript>"
        "<title>%s</title>"
        "<style>"
        "body{font-family:system-ui,-apple-system,sans-serif;background:#0f172a;color:#f8fafc;"
        "display:flex;align-items:center;justify-content:center;min-height:100vh;margin:0;padding:20px;box-sizing:border-box}"
        ".c{background:#1e293b;border:1px solid #334155;border-radius:20px;padding:32px;max-width:440px;width:100%%;"
        "text-align:center;box-shadow:0 20px 25px -5px rgba(0,0,0,0.5)}"
        "h2{margin:10px 0 8px;font-size:20px;color:#38bdf8}"
        "p{color:#94a3b8;font-size:14px;margin:8px 0 18px;line-height:1.5}"
        ".l{width:36px;height:36px;border:3px solid #334155;border-top-color:#38bdf8;border-radius:50%%;"
        "animation:s 1s linear infinite;margin:0 auto 16px}"
        "@keyframes s{to{transform:rotate(360deg)}}"
        ".st{background:#0f172a;padding:10px;border-radius:10px;font-family:monospace;font-size:12px;color:#a7f3d0;margin-bottom:18px}"
        ".b{display:inline-block;padding:10px 20px;background:#2563eb;color:#fff;text-decoration:none;border-radius:8px;"
        "font-weight:600;font-size:13px}"
        "</style></head><body>"
        "<div class='c'><div style='font-size:40px'>⚡</div><div class='l'></div>"
        "<h2>%s</h2>"
        "<p>Se envió la señal de encendido (WOL). Conectando en cuanto el servidor esté listo...</p>"
        "<div class='st' id='st'>Iniciando servidor...</div>"
        "<a class='b' href='%s'>Entrar directo</a></div>"
        "<script>"
        "const t='%s';"
        // El splash pregunta a /ready, que es barato (solo RAM, cero sondas de
        // red) y devuelve el latch de readiness que mantiene la task de
        // ready_probe. Antes preguntaba a /diag y cada vuelta gastaba hasta
        // 500 ms en dos connect() con timeout DENTRO del worker de httpd.
        "function c(){fetch('/ready',{cache:'no-store'}).then(r=>r.json()).then(d=>{"
        "if(d.ready){"
        "document.getElementById('st').innerText='¡Servidor listo! Conectando...';"
        "setTimeout(()=>{window.location.href=t},250);"
        "}else{"
        "var e=(d.state==='ON')?'Levantando el sistema del negocio...':"
        "(d.wake_pending?('Despertando servidor... (WOL intento '+d.wake_attempts+')'):'Iniciando servidor...');"
        "document.getElementById('st').innerText=e;"
        "setTimeout(c,1000)}"
        "}).catch(()=>setTimeout(c,1500))}"
        "setTimeout(c,500);"
        "</script></body></html>",
        title, title, target_url, target_url);

    if (len <= 0 || len >= 2500) {
        free(buf);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, buf, len);
    free(buf);
    return ESP_OK;
}

static esp_err_t handle_gate(httpd_req_t *req) {
    ESP_LOGI(TAG, "GET /gate");
    automation_kick();

    // Determine redirect target based on which server received the request
    const char *location;
    char buf[192];

    if (req->handle == s_server_funnel && strlen(CONFIG_SERVER_FUNNEL_URL) > 0) {
        // Request came via Tailscale Funnel (:443) — redirect to server Funnel URL
        location = CONFIG_SERVER_FUNNEL_URL;
    } else {
        // Request came via LAN/Tailscale direct (:80) — redirect to server Tailscale IP
        snprintf(buf, sizeof(buf), "http://%s:%u/",
                 CONFIG_SERVER_TS_IP, (unsigned)8080);
        location = buf;
    }

    server_state_t st = state_get();
    bool tcp_up = server_verified_alive(st, state_last_poll_tcp_alive());

    if (!tcp_up) {
        ESP_LOGI(TAG, "gate: server down or not verified ON (st=%s), "
                      "pidiendo despertar al supervisor",
                 server_state_str(st));
        wake_request("gate");
        return send_wake_splash(req, location, "Iniciando server (/gate)...");
    }

    ESP_LOGI(TAG, "gate: alive, direct redirect -> %s", location);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ─── /monitor: wake-on-demand + redirect to Monitor (:8085) ──────

static esp_err_t handle_monitor(httpd_req_t *req) {
    ESP_LOGI(TAG, "GET /monitor (redirect to Monitor)");
    automation_kick();

    char location[192];
#ifdef CONFIG_SERVER_MONITOR_PORT
    unsigned port = (unsigned)CONFIG_SERVER_MONITOR_PORT;
#else
    unsigned port = 8085;
#endif
    snprintf(location, sizeof(location), "http://%s:%u/", CONFIG_SERVER_TS_IP, port);

    server_state_t st = state_get();
    bool tcp_up = server_verified_alive(st, state_last_poll_tcp_alive());

    if (!tcp_up) {
        ESP_LOGI(TAG, "monitor: server down or not verified ON (st=%s), "
                      "pidiendo despertar al supervisor",
                 server_state_str(st));
        wake_request("monitor");
        return send_wake_splash(req, location, "Iniciando Monitor (:8085)...");
    }

    ESP_LOGI(TAG, "monitor: alive, redirect -> %s", location);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ─── /app: path-based redirect to server Funnel (with WOL) ──────────

static esp_err_t handle_app(httpd_req_t *req) {
    ESP_LOGI(TAG, "GET /app");
    automation_kick();

    if (strlen(CONFIG_GATE_APP_URL) == 0) {
        ESP_LOGW(TAG, "app: GATE_APP_URL not configured, returning 503");
        send_text(req, "503 Service Unavailable",
                  "app redirect not configured (GATE_APP_URL empty)\n");
        return ESP_OK;
    }

    // Strip /app prefix from req->uri, append rest to CONFIG_GATE_APP_URL.
    // Ej: req->uri = "/app" → rest = "/" → "https://server.tailXXXX.ts.net/"
    //     req->uri = "/app/admin/qr" → rest = "/admin/qr" → "https://.../admin/qr"
    const char *uri = req->uri;
    const char *rest = (strncmp(uri, "/app", 7) == 0) ? uri + 7 : uri;
    size_t base_len = strlen(CONFIG_GATE_APP_URL);
    bool base_has_slash = (base_len > 0 && CONFIG_GATE_APP_URL[base_len - 1] == '/');
    if (*rest == '/' && base_has_slash) {
        rest++;
    } else if (*rest == '\0' && !base_has_slash) {
        rest = "/";
    }

    char loc[600];
    snprintf(loc, sizeof(loc), "%s%s", CONFIG_GATE_APP_URL, rest);

    // Auto-WOL: si server no está confirmado como ON y respondiendo TCP, enviar ráfaga WOL y mostrar splash
    server_state_t st = state_get();
    bool tcp_up = server_verified_alive(st, state_last_poll_tcp_alive());
    if (!tcp_up) {
        ESP_LOGI(TAG, "app: server down or not verified ON (st=%s), "
                      "pidiendo despertar al supervisor",
                 server_state_str(st));
        wake_request("app");
        return send_wake_splash(req, loc, "Despertando the web app...");
    }

    ESP_LOGI(TAG, "app: alive, redirect -> %s", loc);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", loc);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ─── /kiosk: path-based redirect to server Funnel (with WOL) ──────────

static esp_err_t handle_kiubart(httpd_req_t *req) {
    ESP_LOGI(TAG, "GET /kiosk");
    automation_kick();

#ifndef CONFIG_GATE_KIUBART_URL
#define CONFIG_GATE_KIUBART_URL "https://server.tailXXXX.ts.net/app"
#endif

    if (strlen(CONFIG_GATE_KIUBART_URL) == 0) {
        ESP_LOGW(TAG, "kiubart: GATE_KIUBART_URL not configured, returning 503");
        send_text(req, "503 Service Unavailable",
                  "kiubart redirect not configured (GATE_KIUBART_URL empty)\n");
        return ESP_OK;
    }

    // Strip /kiosk prefix from req->uri, append rest to CONFIG_GATE_KIUBART_URL.
    // Ej: req->uri = "/kiosk" → rest = "" → "https://server.tailXXXX.ts.net/app"
    //     req->uri = "/kiosk/admin" → rest = "/admin" → "https://.../kiosk/admin"
    const char *uri = req->uri;
    const char *rest = (strncmp(uri, "/kiosk", 8) == 0) ? uri + 8 : uri;
    size_t base_len = strlen(CONFIG_GATE_KIUBART_URL);
    bool base_has_slash = (base_len > 0 && CONFIG_GATE_KIUBART_URL[base_len - 1] == '/');
    if (*rest == '/' && base_has_slash) {
        rest++;
    }

    char loc[600];
    snprintf(loc, sizeof(loc), "%s%s", CONFIG_GATE_KIUBART_URL, rest);

    // Auto-WOL para server: Kiubart corre en server
    server_state_t st = state_get();
    bool tcp_up = server_verified_alive(st, state_last_poll_tcp_alive());
    if (!tcp_up) {
        ESP_LOGI(TAG, "kiubart: server down or not verified ON (st=%s), "
                      "pidiendo despertar al supervisor",
                 server_state_str(st));
        wake_request("kiubart");
        return send_wake_splash(req, loc, "Despertando Kiubart...");
    }

    ESP_LOGI(TAG, "kiubart: alive, redirect -> %s", loc);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", loc);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ─── /schedule: read/modify scheduled daily wake/off (LAN or Funnel) ────

static esp_err_t handle_schedule(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        scheduler_config_t cfg;
        scheduler_get(&cfg);
        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "wake_set", cfg.wake_set);
        cJSON_AddNumberToObject(root, "wake_min", cfg.wake_min);
        cJSON_AddBoolToObject(root, "off_set", cfg.off_set);
        cJSON_AddNumberToObject(root, "off_min", cfg.off_min);
        char st_text[SCHEDULER_STATUS_BUF_SIZE];
        scheduler_status_str(st_text, sizeof(st_text));
        cJSON_AddStringToObject(root, "status", st_text);
        char *body = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (body == NULL) { httpd_resp_send_500(req); return ESP_FAIL; }
        send_json(req, body);
        free(body);
        return ESP_OK;
    }

    // POST: {"action": "reset"} or {"action": "clear_wake"}
    if (req->content_len <= 0 || req->content_len > 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body length");
        return ESP_FAIL;
    }
    char buf[512];
    int total = 0;
    while (total < req->content_len) {
        int n = httpd_req_recv(req, buf + total, req->content_len - total);
        if (n <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv"); return ESP_FAIL; }
        total += n;
    }
    buf[total] = 0;
    cJSON *doc = cJSON_Parse(buf);
    if (!doc) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json"); return ESP_FAIL; }
    const cJSON *act = cJSON_GetObjectItemCaseSensitive(doc, "action");
    if (act && cJSON_IsString(act) && act->valuestring) {
        if (strcmp(act->valuestring, "reset") == 0) {
            scheduler_reset();
            cJSON_Delete(doc);
            send_text(req, "200 OK", "schedule reset OK (all cleared)\n");
            return ESP_OK;
        } else if (strcmp(act->valuestring, "clear_wake") == 0) {
            scheduler_clear_wake();
            cJSON_Delete(doc);
            send_text(req, "200 OK", "wake schedule cleared OK\n");
            return ESP_OK;
        }
    }
    cJSON_Delete(doc);
    send_text(req, "400 Bad Request", "unknown action (use 'reset' or 'clear_wake')\n");
    return ESP_OK;
}

// ─── /autoset: read/configure the inactivity automation (LAN only) ─────

static esp_err_t handle_autoset(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        automation_config_t cfg;
        automation_get_config(&cfg);
        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "enabled", cfg.enabled);
        cJSON_AddNumberToObject(root, "suspend_after_min", cfg.suspend_after_min);
        cJSON_AddNumberToObject(root, "off_after_min", cfg.off_after_min);
        cJSON_AddNumberToObject(root, "quiet_start_min", cfg.quiet_start_min);
        cJSON_AddNumberToObject(root, "quiet_end_min", cfg.quiet_end_min);
        cJSON_AddStringToObject(root, "state", server_state_str(state_get()));
        cJSON_AddNumberToObject(root, "kick_ok", 1);
        char *body = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (body == NULL) { httpd_resp_send_500(req); return ESP_FAIL; }
        send_json(req, body);
        free(body);
        return ESP_OK;
    }

    // HTTP_POST: body JSON {"enabled":0|1,"suspend_after_min":N,"off_after_min":N,
    //                       "quiet_start_min":N,"quiet_end_min":N}
    if (req->content_len <= 0 || req->content_len > 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
        return ESP_FAIL;
    }
    char *body = malloc(req->content_len + 1);
    if (body == NULL) { httpd_resp_send_500(req); return ESP_ERR_NO_MEM; }
    int total = 0;
    while (total < req->content_len) {
        int n = httpd_req_recv(req, body + total, req->content_len - total);
        if (n <= 0) { free(body); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv"); return ESP_FAIL; }
        total += n;
    }
    body[total] = 0;
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad JSON"); return ESP_FAIL; }

    automation_config_t cfg;
    automation_get_config(&cfg);

    const cJSON *e = cJSON_GetObjectItem(root, "enabled");
    if (e != NULL && cJSON_IsBool(e)) cfg.enabled = cJSON_IsTrue(e);
    else if (e != NULL && cJSON_IsNumber(e)) cfg.enabled = (e->valueint != 0);

    const cJSON *sus = cJSON_GetObjectItem(root, "suspend_after_min");
    if (sus != NULL && cJSON_IsNumber(sus) && sus->valueint >= 0 && sus->valueint <= 1440)
        cfg.suspend_after_min = (uint16_t)sus->valueint;

    const cJSON *off = cJSON_GetObjectItem(root, "off_after_min");
    if (off != NULL && cJSON_IsNumber(off) && off->valueint >= 0 && off->valueint <= 1440)
        cfg.off_after_min = (uint16_t)off->valueint;

    const cJSON *qs = cJSON_GetObjectItem(root, "quiet_start_min");
    if (qs != NULL && cJSON_IsNumber(qs) && qs->valueint >= 0 && qs->valueint < 1440)
        cfg.quiet_start_min = (uint16_t)qs->valueint;

    const cJSON *qe = cJSON_GetObjectItem(root, "quiet_end_min");
    if (qe != NULL && cJSON_IsNumber(qe) && qe->valueint >= 0 && qe->valueint < 1440)
        cfg.quiet_end_min = (uint16_t)qe->valueint;

    cJSON_Delete(root);
    esp_err_t err = automation_set_config(&cfg);
    if (err != ESP_OK) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set"); return ESP_FAIL; }
    send_text(req, "200 OK", "autoset saved\n");
    ESP_LOGI(TAG, "autoset POST: en=%d sus=%u off=%u q=[%u..%u)",
             (int)cfg.enabled, (unsigned)cfg.suspend_after_min,
             (unsigned)cfg.off_after_min, (unsigned)cfg.quiet_start_min,
             (unsigned)cfg.quiet_end_min);
    return ESP_OK;
}

// ─── /diag: diagnostic endpoint ─────────────────────────────────────────

#if CONFIG_ESP_HTTPS_SERVER_ENABLE





static void diag_cert_summary(char *out, size_t out_size) {
    const char *subj = "n/a", *nbf = "n/a", *na = "n/a";
    char subj_buf[160], nbf_buf[48], na_buf[48];

    uint8_t *cert = NULL, *key = NULL;
    size_t cert_len = 0, key_len = 0;
    if (ml_cert_load(&cert, &cert_len, &key, &key_len) == ESP_OK) {
        mbedtls_x509_crt crt;
        mbedtls_x509_crt_init(&crt);
        if (mbedtls_x509_crt_parse(&crt, cert, cert_len + 1) == 0) {
            if (mbedtls_x509_dn_gets(subj_buf, sizeof(subj_buf), &crt.subject) > 0) {
                subj = subj_buf;
            }
            snprintf(nbf_buf, sizeof(nbf_buf), "%04u-%02u-%02u %02u:%02u:%02u",
                     crt.valid_from.year, crt.valid_from.mon, crt.valid_from.day,
                     crt.valid_from.hour, crt.valid_from.min, crt.valid_from.sec);
            snprintf(na_buf, sizeof(na_buf), "%04u-%02u-%02u %02u:%02u:%02u",
                     crt.valid_to.year, crt.valid_to.mon, crt.valid_to.day,
                     crt.valid_to.hour, crt.valid_to.min, crt.valid_to.sec);
            nbf = nbf_buf;
            na = na_buf;
        }
        mbedtls_x509_crt_free(&crt);
        free(cert);
        free(key);
    }

    snprintf(out, out_size, "subject=%s, not_before=%s GMT, not_after=%s GMT, source=%s",
             subj, nbf, na, web_server_funnel_tls_cert_source());
}
#endif 









static esp_err_t handle_ready(httpd_req_t *req) {
    server_state_t st = state_get();
    bool ready = server_ready_cached();

    wake_status_t ws;
    wake_get_status(&ws);
    ready_status_t rs;
    ready_get_status(&rs);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ready", ready);
    cJSON_AddStringToObject(root, "state", server_state_str(st));
    cJSON_AddBoolToObject(root, "wake_pending", ws.pending);
    cJSON_AddNumberToObject(root, "wake_attempts", ws.attempts);
    cJSON_AddNumberToObject(root, "wake_elapsed_ms", ws.elapsed_ms);
    cJSON_AddStringToObject(root, "wake_status", ws.text);
    cJSON_AddStringToObject(root, "ready_status", ready_status_text());
    cJSON_AddStringToObject(root, "ready_source", s_ready_source);
    cJSON_AddNumberToObject(root, "ready_ms_ago", rs.ready_ms_ago);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) { httpd_resp_send_500(req); return ESP_FAIL; }
    send_json(req, body);
    free(body);
    return ESP_OK;
}




static bool notify_secret_ok(httpd_req_t *req) {
    if (strlen(CONFIG_ESP32_READY_NOTIFY_SECRET) == 0) {
        ESP_LOGW(TAG, "/notify deshabilitado: CONFIG_ESP32_READY_NOTIFY_SECRET vacio");
        return false;
    }
    char got[96] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-ESP32-Ready-Secret",
                                    got, sizeof(got)) != ESP_OK || got[0] == 0) {
        ESP_LOGW(TAG, "/notify/ready sin cabecera de secreto (ip %s)",
                 gate_peer_ip(req) ? gate_peer_ip(req) : "?");
        return false;
    }
    if (strcmp(got, CONFIG_ESP32_READY_NOTIFY_SECRET) != 0) {
        ESP_LOGW(TAG, "/notify/ready con secreto incorrecto (ip %s)",
                 gate_peer_ip(req) ? gate_peer_ip(req) : "?");
        return false;
    }
    return true;
}



static esp_err_t handle_notify_ready(httpd_req_t *req) {
    if (!notify_secret_ok(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "notify/ready: secreto invalido", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    const char *peer = gate_peer_ip(req);
    ESP_LOGI(TAG, "/notify/ready desde %s: server declara su stack listo",
             peer ? peer : "?");
    ready_notify_ready();
    send_text(req, "200 OK", "ready: latch encendido. El portal redirige al instante.\n");
    return ESP_OK;
}










static esp_err_t handle_notify_unready(httpd_req_t *req) {
    if (!notify_secret_ok(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "notify/unready: secreto invalido", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    const char *peer = gate_peer_ip(req);
    ESP_LOGW(TAG, "/notify/unready desde %s: server declara que su ruta publica no sirve",
             peer ? peer : "?");
    ready_notify_unready();
    send_text(req, "200 OK", "ready: latch apagado y gate cerrado. El portal vuelve a esperar.\n");
    return ESP_OK;
}


static esp_err_t handle_diag(httpd_req_t *req) {
    ESP_LOGI(TAG, "GET /diag");
    automation_kick();

    server_state_t st = state_get();
    const char *peer_ip = gate_peer_ip(req);

    cJSON *root = cJSON_CreateObject();

    // Firmware
    cJSON_AddStringToObject(root, "firmware", FIRMWARE_VERSION);

    // server state
    cJSON_AddStringToObject(root, "server_state", server_state_str(st));
    cJSON_AddStringToObject(root, "server_ip", CONFIG_SERVER_IP);
    cJSON_AddStringToObject(root, "server_ts_ip", CONFIG_SERVER_TS_IP);
    // Si state != ON y no hay fast-poll activo, usar el resultado cacheado del poll
    // para no bloquear la tarea monohilo httpd con timeouts TCP.
    bool tcp_alive = (st == SERVER_STATE_ON || state_is_fast_polling()) ? server_tcp_alive(250) : state_last_poll_tcp_alive();
    bool web_alive = (st == SERVER_STATE_ON || tcp_alive) ? server_web_alive(250) : false;
    cJSON_AddBoolToObject(root, "server_tcp_alive", tcp_alive);
    cJSON_AddBoolToObject(root, "server_web_alive", web_alive);
    bool stack_ok = state_stack_settled(SERVER_STACK_SETTLE_MS);
    bool server_up = web_alive || (st == SERVER_STATE_ON && tcp_alive);
    // ready = el servidor responde Y su stack publicable (nginx + Funnel)
    // puede recibir ya al cliente. Sin el gate de stack, el hueco medido de
    // ~20 s tras un arranque en frio devolvia 502 al cliente.
    cJSON_AddBoolToObject(root, "server_ready", server_up && server_target_ready());
    cJSON_AddStringToObject(root, "ready_source", s_ready_source);

    // Sonda de readiness (ready_probe.c): con que respuesta real se cerro el
    // latch y cuando fue. Es el numero que hay que mirar para saber si el portal
    // esta esperando por algo real o por el techo de seguridad.
    {
        ready_status_t rs;
        ready_get_status(&rs);
        cJSON_AddBoolToObject(root, "ready_latched", rs.latched);
        cJSON_AddBoolToObject(root, "ready_armed", rs.armed);
        cJSON_AddNumberToObject(root, "ready_ms_ago", rs.ready_ms_ago);
        cJSON_AddNumberToObject(root, "ready_probes", rs.probes);
        cJSON_AddNumberToObject(root, "ready_ok", rs.ok);
        cJSON_AddNumberToObject(root, "ready_err", rs.err);
        cJSON_AddNumberToObject(root, "ready_last_duration_ms", rs.last_duration_ms);
        cJSON_AddNumberToObject(root, "ready_last_status", rs.last_status);
        cJSON_AddNumberToObject(root, "ready_last_api_status", rs.last_api_status);
        cJSON_AddStringToObject(root, "ready_status", ready_status_text());
    }
    cJSON_AddBoolToObject(root, "server_stack_ok", stack_ok);
    cJSON_AddBoolToObject(root, "server_boot_verified", state_boot_verified_live());
    {
        uint32_t s_att = 0, s_ok = 0, s_err = 0; int32_t s_len = -1;
        state_http_stats(&s_att, &s_ok, &s_err, &s_len);
        cJSON_AddNumberToObject(root, "server_state_http_attempts", s_att);
        cJSON_AddNumberToObject(root, "server_state_http_ok", s_ok);
        cJSON_AddNumberToObject(root, "server_state_http_err", s_err);
        cJSON_AddNumberToObject(root, "server_state_http_last_len", s_len);
    }
    cJSON_AddNumberToObject(root, "server_uptime_s", state_server_uptime_s());

    // Supervisor de Auto-WOL (wake.c). Antes el WOL era un disparo unico sin
    // ninguna traza: cuando la rafaga se perdia no habia forma de saber si el
    // ESP32 la habia enviado, cuantas veces habia reintentado ni cuando
    // confirmo que el server respondio. Esto lo hace verificable por HTTP.
    {
        wake_status_t ws;
        wake_get_status(&ws);
        cJSON_AddBoolToObject(root, "wake_pending", ws.pending);
        cJSON_AddNumberToObject(root, "wake_attempts", ws.attempts);
        cJSON_AddNumberToObject(root, "wake_elapsed_ms", ws.elapsed_ms);
        cJSON_AddNumberToObject(root, "wake_retry_s", ws.retry_ms / 1000);
        cJSON_AddNumberToObject(root, "wake_window_s", ws.window_ms / 1000);
        cJSON_AddStringToObject(root, "wake_status", ws.text);
    }

    // Network
    cJSON_AddStringToObject(root, "client_ip", peer_ip ? peer_ip : "unknown");
    cJSON_AddBoolToObject(root, "client_is_lan", gate_is_lan_peer(peer_ip));

    // Tailscale
    microlink_state_t ml_state = microlink_wrapper_get_state();
    cJSON_AddBoolToObject(root, "tailscale_connected", microlink_wrapper_is_connected());
    cJSON_AddStringToObject(root, "tailscale_state", microlink_wrapper_state_name(ml_state));
    uint32_t vpn_ip = microlink_wrapper_get_vpn_ip();
    char vpn_ip_str[16] = {0};
    if (vpn_ip) {
        snprintf(vpn_ip_str, sizeof(vpn_ip_str), "%lu.%lu.%lu.%lu",
                 (vpn_ip >> 24) & 0xFF, (vpn_ip >> 16) & 0xFF,
                 (vpn_ip >> 8) & 0xFF, vpn_ip & 0xFF);
    }
    cJSON_AddStringToObject(root, "tailscale_vpn_ip", vpn_ip_str);

    // Watchdog
    microlink_wd_status_t wd;
    microlink_watchdog_get_status(&wd);
    cJSON_AddBoolToObject(root, "tailscale_watchdog_enabled", wd.enabled);
    cJSON_AddBoolToObject(root, "tailscale_watchdog_ever_connected", wd.ever_connected);
    cJSON_AddNumberToObject(root, "tailscale_watchdog_disconnect_ms", wd.disconnect_ms);
    cJSON_AddNumberToObject(root, "tailscale_watchdog_ctrl_rx_ms_ago", wd.ctrl_rx_ms_ago);
    cJSON_AddNumberToObject(root, "tailscale_watchdog_total_recoveries", wd.total_recoveries);
    cJSON_AddNumberToObject(root, "tailscale_watchdog_total_rebinds", wd.total_rebinds);
    cJSON_AddStringToObject(root, "tailscale_watchdog_last_recovery",
        wd.last_recovery_kind == ML_WD_RECOVERY_REBOOT ? "reboot" :
        wd.last_recovery_kind == ML_WD_RECOVERY_REBIND ? "rebind" : "none");
    cJSON_AddNumberToObject(root, "tailscale_watchdog_last_recovery_ms_ago",
                            wd.last_recovery_ms_ago);
    cJSON_AddNumberToObject(root, "tailscale_watchdog_last_recovery_target_s",
                            wd.last_recovery_target_s);
    cJSON_AddNumberToObject(root, "tailscale_watchdog_last_rebind_ms_ago",
                            wd.last_rebind_ms_ago);

    // Memory
    cJSON_AddNumberToObject(root, "heap_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "psram_free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // Portmap info
    cJSON_AddNumberToObject(root, "portmap_count", portmap_count());

#if CONFIG_ESP_HTTPS_SERVER_ENABLE
    // Cert TLS funnel (sujeto + validez) — para descartar/confirmar
    // problemas de cert sin tocar serial (histórico sesión 19).
    {
        char cert_summary[384];
        diag_cert_summary(cert_summary, sizeof(cert_summary));
        cJSON_AddStringToObject(root, "funnel_cert", cert_summary);
    }
#endif

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    send_json(req, body);
    free(body);
    return ESP_OK;
}

// ─── URI tables ──────────────────────────────────────────────────────────────







static const httpd_uri_t s_uris_public[] = {
    { .uri = "/",        .method = HTTP_GET, .handler = handle_root   },
    { .uri = "/wol",     .method = HTTP_GET, .handler = handle_wol    },
    { .uri = "/status",  .method = HTTP_GET, .handler = handle_status },
    { .uri = "/test_tg", .method = HTTP_GET, .handler = handle_test_tg},
    { .uri = "/gate",     .method = HTTP_GET, .handler = handle_gate    },
    { .uri = "/monitor*", .method = HTTP_GET, .handler = handle_monitor },
    { .uri = "/app*",  .method = HTTP_GET, .handler = handle_app  },
    { .uri = "/kiosk*", .method = HTTP_GET, .handler = handle_kiubart },
      { .uri = "/ready",   .method = HTTP_GET, .handler = handle_ready  },
      { .uri = "/diag",    .method = HTTP_GET, .handler = handle_diag   },
    { .uri = "/portmap", .method = HTTP_GET, .handler = handle_portmap},
    { .uri = "/schedule",.method = HTTP_GET, .handler = handle_schedule},
};

#if CONFIG_ESP_HTTPS_SERVER_ENABLE

static esp_err_t handle_api_cert_set(httpd_req_t *req);
static esp_err_t handle_api_cert_info(httpd_req_t *req);
static esp_err_t handle_api_cert_delete(httpd_req_t *req);



static const httpd_uri_t s_uris_private[] = {
    { .uri = "/api/cert/set",  .method = HTTP_POST,   .handler = handle_api_cert_set },
    { .uri = "/api/cert/info", .method = HTTP_GET,    .handler = handle_api_cert_info },
    { .uri = "/api/cert",      .method = HTTP_DELETE, .handler = handle_api_cert_delete },
    { .uri = "/autoset",       .method = HTTP_GET,    .handler = handle_autoset },
      { .uri = "/notify/ready", .method = HTTP_GET,  .handler = handle_notify_ready },
      { .uri = "/notify/unready", .method = HTTP_GET,  .handler = handle_notify_unready },
    { .uri = "/autoset",       .method = HTTP_POST,   .handler = handle_autoset },
    { .uri = "/schedule",      .method = HTTP_POST,   .handler = handle_schedule },
};
#endif

static esp_err_t register_uris_public(httpd_handle_t server, const char *label) {
    for (size_t i = 0; i < sizeof(s_uris_public) / sizeof(s_uris_public[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &s_uris_public[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register public %s on %s failed: %s",
                     s_uris_public[i].uri, label, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

#if CONFIG_ESP_HTTPS_SERVER_ENABLE
static esp_err_t register_uris_private(httpd_handle_t server, const char *label) {
    for (size_t i = 0; i < sizeof(s_uris_private) / sizeof(s_uris_private[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &s_uris_private[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register private %s on %s failed: %s",
                     s_uris_private[i].uri, label, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}
#endif

// ─── HTTP server :80 (LAN + Tailscale) ──────────────────────────────────

esp_err_t web_server_start(void) {
    if (s_server != NULL) {
        ESP_LOGW(TAG, "server :80 already running");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 8192;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;  
    config.max_uri_handlers = 20;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start :80 failed: %s", esp_err_to_name(err));
        return err;
    }

    err = register_uris_public(s_server, ":80");
#if CONFIG_ESP_HTTPS_SERVER_ENABLE
    if (err == ESP_OK) err = register_uris_private(s_server, ":80 (private)");
#endif
    if (err != ESP_OK) {
        httpd_stop(s_server);
        s_server = NULL;
        return err;
    }

    ESP_LOGI(TAG, "HTTP server listening on :80 (LAN + Tailscale)");
    return ESP_OK;
}

// ─── HTTP server :443 (Tailscale Funnel — plaintext, no TLS) ────────────

esp_err_t web_server_funnel_start(void) {
    if (s_server_funnel != NULL) {
        ESP_LOGW(TAG, "funnel server :443 already running");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 443;
    config.stack_size = 8192;
    config.ctrl_port += 1;  // different from :80 to avoid collision
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;  
    config.max_uri_handlers = 20;

    esp_err_t err = httpd_start(&s_server_funnel, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start :443 failed: %s", esp_err_to_name(err));
        return err;
    }

    
    err = register_uris_public(s_server_funnel, ":443");
    if (err != ESP_OK) {
        httpd_stop(s_server_funnel);
        s_server_funnel = NULL;
        return err;
    }

    ESP_LOGI(TAG, "Funnel server listening on :443 (plaintext for Tailscale Funnel)");
    return ESP_OK;
}

esp_err_t web_server_stop(void) {
    if (s_server == NULL) return ESP_OK;
    esp_err_t err = httpd_stop(s_server);
    s_server = NULL;
    return err;
}

esp_err_t web_server_funnel_stop(void) {
    if (s_server_funnel == NULL) return ESP_OK;
    esp_err_t err = httpd_stop(s_server_funnel);
    s_server_funnel = NULL;
    return err;
}

// ─── HTTPS server :443 (Tailscale Funnel — TLS terminado) ──────────────────
//
// Carga cert de NVS primero (emitido por ACME client), fallback a cert
// autofirmado hardcoded. Cuando llega un cert nuevo vía POST /api/cert/set,
// llama a web_server_funnel_tls_reload() para recargar sin reiniciar el ESP32.

#if CONFIG_ESP_HTTPS_SERVER_ENABLE


static const char *s_cert_source = "none";  // "nvs", "fallback-hardcoded"

esp_err_t web_server_funnel_tls_start(void) {
    if (s_server_funnel != NULL) {
        ESP_LOGW(TAG, "funnel server :443 already running");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting Funnel HTTPS server on :443 (TLS)...");

    
    uint8_t *cert_copy = NULL, *key_copy = NULL;
    size_t cert_len = 0, key_len = 0;
    esp_err_t err = ml_cert_load(&cert_copy, &cert_len, &key_copy, &key_len);
    if (err == ESP_OK) {
        s_cert_source = "nvs";
        ESP_LOGI(TAG, "Using cert from NVS (cert=%u bytes, key=%u bytes)",
                 (unsigned)cert_len, (unsigned)key_len);
        



        cert_len += 1;
        key_len += 1;
    } else {
        s_cert_source = "fallback-hardcoded";
        ESP_LOGW(TAG, "No cert in NVS (%s), using self-signed fallback — "
                      "edge will NOT trust this until ACME cert is issued",
                 esp_err_to_name(err));
        cert_copy = (uint8_t *)acme_test_cert_der;
        cert_len = acme_test_cert_der_len;
        key_copy = (uint8_t *)acme_test_key_der;
        key_len = acme_test_key_der_len;
    }

    httpd_ssl_config_t cfg = HTTPD_SSL_CONFIG_DEFAULT();
    cfg.httpd.server_port = 443;
    cfg.httpd.ctrl_port += 1;  // diferente de :80 para evitar colisión
    cfg.httpd.max_open_sockets = 4;
    cfg.httpd.lru_purge_enable = true;
    cfg.httpd.uri_match_fn = httpd_uri_match_wildcard;  
    cfg.httpd.max_uri_handlers = 20;

    



    static uint8_t *s_heap_cert = NULL, *s_heap_key = NULL;

    cfg.servercert = cert_copy;
    cfg.servercert_len = cert_len;
    cfg.prvtkey_pem = key_copy;
    cfg.prvtkey_len = key_len;

    err = httpd_ssl_start(&s_server_funnel, &cfg);

    

    if (s_cert_source[0] == 'n' && cert_copy != (uint8_t *)acme_test_cert_der) {
        free(s_heap_cert);
        free(s_heap_key);
        s_heap_cert = cert_copy;
        s_heap_key = key_copy;
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ssl_start :443 failed: %s", esp_err_to_name(err));
        s_server_funnel = NULL;
        return err;
    }

    err = register_uris_public(s_server_funnel, ":443 (TLS)");
    if (err != ESP_OK) {
        httpd_ssl_stop(s_server_funnel);
        s_server_funnel = NULL;
        return err;
    }

    ESP_LOGI(TAG, "Funnel HTTPS server listening on :443 (TLS), cert source: %s",
             s_cert_source);
    return ESP_OK;
}

esp_err_t web_server_funnel_tls_stop(void) {
    if (s_server_funnel == NULL) return ESP_OK;
    esp_err_t err = httpd_ssl_stop(s_server_funnel);
    s_server_funnel = NULL;
    return err;
}



esp_err_t web_server_funnel_tls_reload(void) {
    ESP_LOGI(TAG, "Reloading TLS server with new cert from NVS...");
    esp_err_t err = web_server_funnel_tls_stop();
    if (err != ESP_OK) return err;
    return web_server_funnel_tls_start();
}

const char *web_server_funnel_tls_cert_source(void) {
    return s_cert_source;
}









static esp_err_t handle_api_cert_set(httpd_req_t *req) {
    if (req->content_len == 0 || req->content_len > 16384) {
        ESP_LOGE(TAG, "api/cert/set: bad content_len=%d", req->content_len);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid content length");
        return ESP_FAIL;
    }

    char *body = malloc(req->content_len + 1);
    if (body == NULL) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    

    int recv_len = 0;
    int total_len = 0;
    int remaining = req->content_len;
    while (remaining > 0) {
        recv_len = httpd_req_recv(req, body + total_len, remaining);
        if (recv_len <= 0) {
            ESP_LOGE(TAG, "api/cert/set: recv failed (got %d of %u)",
                     recv_len, req->content_len);
            free(body);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_FAIL;
        }
        total_len += recv_len;
        remaining -= recv_len;
    }
    body[total_len] = 0;

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL) {
        ESP_LOGE(TAG, "api/cert/set: invalid JSON");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    cJSON *cert_j = cJSON_GetObjectItem(root, "cert");
    cJSON *key_j  = cJSON_GetObjectItem(root, "key");
    if (!cJSON_IsString(cert_j) || cert_j->valuestring == NULL ||
        !cJSON_IsString(key_j)  || key_j->valuestring  == NULL) {
        ESP_LOGE(TAG, "api/cert/set: missing cert/key fields");
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing cert or key");
        return ESP_FAIL;
    }

    const char *cert_in = cert_j->valuestring;
    const char *key_in  = key_j->valuestring;

    


    uint8_t *cert_der = NULL;
    size_t cert_der_len = 0;
    uint8_t *key_der = NULL;
    size_t key_der_len = 0;
    esp_err_t conv_err;

    ESP_LOGI(TAG, "api/cert/set: received cert=%u chars, key=%u chars",
             (unsigned)strlen(cert_in), (unsigned)strlen(key_in));

    
    if (strncmp(cert_in, "-----BEGIN", 10) == 0) {
        ESP_LOGI(TAG, "  cert is PEM, converting to DER");
        conv_err = ml_pem_to_der(cert_in, strlen(cert_in), &cert_der, &cert_der_len);
        if (conv_err != ESP_OK) {
            ESP_LOGE(TAG, "  cert PEM→DER failed: %s", esp_err_to_name(conv_err));
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid PEM cert");
            return ESP_FAIL;
        }
    } else {
        cert_der = (uint8_t *)cert_in;
        cert_der_len = strlen(cert_in);
    }
    if (strncmp(key_in, "-----BEGIN", 10) == 0) {
        ESP_LOGI(TAG, "  key is PEM, converting to DER");
        conv_err = ml_pem_to_der(key_in, strlen(key_in), &key_der, &key_der_len);
        if (conv_err != ESP_OK) {
            ESP_LOGE(TAG, "  key PEM→DER failed: %s", esp_err_to_name(conv_err));
            free(cert_der);
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid PEM key");
            return ESP_FAIL;
        }
    } else {
        key_der = (uint8_t *)key_in;
        key_der_len = strlen(key_in);
    }

    ESP_LOGI(TAG, "  storing DER: cert=%u bytes, key=%u bytes",
             (unsigned)cert_der_len, (unsigned)key_der_len);

    esp_err_t err = ml_cert_store(cert_der, cert_der_len,
                                   key_der,  key_der_len);
    
    if (cert_der != (uint8_t *)cert_in) free(cert_der);
    if (key_der  != (uint8_t *)key_in)  free(key_der);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ml_cert_store failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "store failed");
        return ESP_FAIL;
    }

    
    err = web_server_funnel_tls_reload();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TLS reload failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "reload failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}\n", 15);
    return ESP_OK;
}



static esp_err_t handle_api_cert_info(httpd_req_t *req) {
    bool present = false;
    size_t cert_len = 0, key_len = 0;
    ml_cert_info(&present, &cert_len, &key_len);

    char body[256];
    int n = snprintf(body, sizeof(body),
        "{\"present\":%s,\"source\":\"%s\",\"cert_len\":%u,\"key_len\":%u}\n",
        present ? "true" : "false",
        web_server_funnel_tls_cert_source(),
        (unsigned)cert_len, (unsigned)key_len);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, n);
    return ESP_OK;
}



static esp_err_t handle_api_cert_delete(httpd_req_t *req) {
    esp_err_t err = ml_cert_clear();
    if (err != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    web_server_funnel_tls_reload();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"cleared\"}\n", 19);
    return ESP_OK;
}

#endif // CONFIG_ESP_HTTPS_SERVER_ENABLE