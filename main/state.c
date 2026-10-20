#include "state.h"

#include <esp_log.h>
#include <esp_http_client.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <fcntl.h>
#include <errno.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <sys/time.h>
#include <nvs_flash.h>

#include "server_client.h"
#include "automation.h"
#include "system_health.h"

#include "sdkconfig.h"

#define NVS_NAMESPACE_STATE   "server"
#define NVS_KEY_BOOT_TIME     "boot_time"
#define NVS_KEY_SUSPECT_SUSP  "suspect_susp"
#define NVS_KEY_LAST_ALIVE    "last_alive"

static const char *TAG = "state";

static volatile server_state_t g_state = SERVER_STATE_UNKNOWN;
static volatile uint32_t g_state_entered_ms = 0;
static volatile uint32_t g_last_known_on_ms = 0;
// Ultimo instante (reloj de pared, unix) en que vimos server con vida.
// Se persiste en NVS para que un reinicio del ESP32 (el auto-reboot es
// horario) no borre el dato: sin el, despues de un reinicio el ESP32 ya no
// sabia que el servidor estaba en S3 y un /off contestaba "already off".
static volatile uint32_t g_last_alive_unix = 0;
static uint32_t s_last_alive_persist_ms = 0;
// true cuando sabemos (o creemos saber) que server esta en S3: lo mandamos
// a suspender, el poll lo vio desaparecer, o el estado se degrado de SUSPEND
// a OFF. Sin este dato, un /off sobre un servidor suspendido contestaba
// "already off" y no apagaba nada (el equipo se quedaba encendido para
// siempre con la Tailscale caida). Persistido en NVS para sobrevivir al
// auto-reboot horario del ESP32.
static volatile bool g_suspect_suspended = false;
static volatile uint32_t g_server_boot_time = 0;
static volatile uint32_t g_last_poll_ms = 0;
static volatile bool g_last_poll_tcp_alive = false;
// Momento (ms) en el que se observo por ultima vez un boot_time DISTINTO al
// que ya teniamos. 0 = no hemos visto ningun arranque nuevo desde el ultimo
// OFF (o sea: un simple resume desde S3, que no necesita espera).
static volatile uint32_t g_boot_changed_ms = 0;
// Periodo ON durante el cual se detecto ese arranque en frio. Si el servidor
// vuelve a ON en un periodo POSTERIOR (p.ej. un resume desde S3 que el poll no
// llego a ver como SUSPEND), la marca vieja no aplica a este periodo.
static volatile uint32_t g_cold_boot_since_on_ms = 0;
// true cuando el uptime que reportamos proviene de una lectura EN VIVO de
// /state hecha en este boot del ESP32 (no del boot_time cacheado en NVS).
static volatile bool g_boot_verified_live = false;
// true mientras el boot_time guardado NO corresponda al periodo ON actual:
// o sea, entre que el server vuelve a responder y que el poll consigue leer
// /state en vivo. Sin esta marca, durante esa ventana el gate de stack creeria
// que no hubo arranque en frio (porque el uptime cacheado parece enorme) y
// mandaria al cliente a un 502. La marca la baja note_boot_time_live().
static volatile bool g_boot_confirmed_for_on = false;
// Diagnostico de la lectura EN VIVO de /state (visible en /diag por HTTP: con
// el log en UART0 no hay forma de verlo sin conectar un cable al UART).
static volatile uint32_t s_state_http_attempts = 0;
static volatile uint32_t s_state_http_ok = 0;
static volatile uint32_t s_state_http_err = 0;
static volatile int32_t  s_state_http_last_len = -1;

#define LAST_ON_HEURISTIC_MS 300000UL

static void persist_boot_time(uint32_t boot_time) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE_STATE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, NVS_KEY_BOOT_TIME, boot_time);
    nvs_commit(h);
    nvs_close(h);
}

static uint32_t load_persisted_boot_time(void) {
    nvs_handle_t h;
    uint32_t v = 0;
    if (nvs_open(NVS_NAMESPACE_STATE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, NVS_KEY_BOOT_TIME, &v);
        nvs_close(h);
    }
    return v;
}

uint32_t state_server_boot_time(void) { return g_server_boot_time; }

// ─── Rastro de suspension ──────────────────────────────────────

static void persist_suspect_suspended(bool v) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE_STATE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_SUSPECT_SUSP, v ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

static void set_suspect_suspended(bool v) {
    if (g_suspect_suspended == v) return;
    g_suspect_suspended = v;
    persist_suspect_suspended(v);
    ESP_LOGI(TAG, "rastro de suspension: %d", (int)v);
}

bool state_suspect_suspended(void) { return g_suspect_suspended; }

void state_mark_suspended(void) { set_suspect_suspended(true); }

void state_clear_suspect_suspended(void) { set_suspect_suspended(false); }

// ─── Ultimo instante en que vimos el servidor con vida ─────────
static void persist_last_alive(uint32_t unix_ts) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE_STATE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, NVS_KEY_LAST_ALIVE, unix_ts);
    nvs_commit(h);
    nvs_close(h);
}

// Se llama cada vez que el servidor responde. Se persiste con throttle de
// 60 s (un commit por minuto en NVS es aceptable con wear leveling) para no
// castigar el flash con un commit cada 30 s.
static void note_alive_now(void) {
    uint32_t now_unix = (uint32_t)time(NULL);
    if (now_unix < 1704067200) return;   // reloj sin sincronizar
    g_last_alive_unix = now_unix;
    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (now_ms - s_last_alive_persist_ms >= 60000UL) {
        s_last_alive_persist_ms = now_ms;
        persist_last_alive(now_unix);
    }
}

// true si vimos al servidor con vida hace poco (< LAST_ON_HEURISTIC_MS),
// sea en este boot (contador de ticks) o en el anterior (reloj de pared).
static bool seen_alive_recently(void) {
    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (g_last_known_on_ms != 0 && (now_ms - g_last_known_on_ms) < LAST_ON_HEURISTIC_MS) {
        return true;
    }
    if (g_last_alive_unix != 0) {
        uint32_t now_unix = (uint32_t)time(NULL);
        if (now_unix > g_last_alive_unix &&
            (now_unix - g_last_alive_unix) < (LAST_ON_HEURISTIC_MS / 1000UL)) {
            return true;
        }
    }
    return false;
}

// Registra un boot_time leido EN VIVO por HTTP. Si difiere del que teniamos,
// server arranco de cero (no es un resume desde S3) y hay que esperar a que
// el stack web levante antes de mandar al cliente.
static void note_boot_time_live(uint32_t new_bt) {
    if (new_bt == 0) return;
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (new_bt != g_server_boot_time) {
        ESP_LOGI(TAG, "server boot_time changed: %u -> %u (arranque en frio, "
                      "esperando a que el stack web levante)",
                 (unsigned)g_server_boot_time, (unsigned)new_bt);
        g_boot_changed_ms = now;
        g_cold_boot_since_on_ms = g_state_entered_ms;   // informativo
        g_server_boot_time = new_bt;
        persist_boot_time(new_bt);
    }
    g_boot_verified_live = true;
    g_boot_confirmed_for_on = true;
}

bool state_boot_verified_live(void) { return g_boot_verified_live; }

uint32_t state_http_stats(uint32_t *attempts, uint32_t *ok, uint32_t *err, int32_t *last_len) {
    if (attempts) *attempts = s_state_http_attempts;
    if (ok)       *ok       = s_state_http_ok;
    if (err)      *err      = s_state_http_err;
    if (last_len) *last_len = s_state_http_last_len;
    return s_state_http_ok;
}

bool state_stack_settled(uint32_t cold_boot_min_ms) {
    if (g_state != SERVER_STATE_ON) return false;
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    if (g_boot_verified_live && g_boot_confirmed_for_on) {
        // No hemos visto boot nuevo -> es un resume desde S3: el stack sigue en
        // pie y no hay que esperar nada.
        if (g_boot_changed_ms == 0) return true;
        // Hubo arranque en frio. OJO: ya NO se puede comparar g_cold_boot_since_on_ms
        // con state_entered_at_ms() para decidir si la espera ya se pago. Durante un
        // arranque en frio el /state se lee por primera vez cuando la maquina vuelve,
        // y en ese instante g_state_entered_ms es la transicion a SUSPEND, no la de ON:
        // la comparacion nunca coincidia y la espera se saltaba entera (medido el
        // 2026-10-15: el gate se abria en el mismo instante en que aparecia ON). El
        // caso de "ya lo pagamos" es ahora state_stack_never_stopped().
        if (g_cold_boot_since_on_ms != state_entered_at_ms()) return true;
        // Arranque en frio: la espera se ancla al momento en que el ESP32
        // VOLVIO a ver el servidor (transicion a ON), no al instante en que
        // leyo el boot_time. Durante el arranque la lectura de /state puede
        // tardar 20-30 s mas (python saturado con Docker arrancando) y usar el
        // boot_time como ancla alargaba la espera del cliente a ~100 s.
        uint32_t anchor = state_entered_at_ms();
        if (anchor == 0) anchor = g_boot_changed_ms;
        return (uint32_t)(now - anchor) >= cold_boot_min_ms;
    }

    // Plan B: todavia no pudimos leer /state en vivo para ESTE periodo ON
    // (el server acaba de volver y el poll aun no consulto, el servicio :9999
    // cayo, o el ESP32 arranco sin red). No podemos saber si fue arranque en
    // frio, asi que aplicamos la espera completa desde la transicion a ON.
    // Es la red de seguridad: el portal NUNCA se queda esperando para siempre
    // por un dato que no llego, y nunca manda al cliente antes de tiempo.
    uint32_t on_ms = state_entered_at_ms();
    if (on_ms == 0) return false;
    return (uint32_t)(now - on_ms) >= (cold_boot_min_ms + 30000UL);
}
// true cuando server NUNCA se apago desde el ultimo OFF: el stack sigue en pie
// y no hay nada que esperar. Es la via rapida (sin esperas) del resume desde S3.
//
// Por que NO basta con state_stack_settled(0): esa funcion exige un minimo de
// espera, asi que con 0 ms devuelve true tambien durante un arranque en frio, que
// es justo cuando NO hay que dar via libre todavia. Aqui se separa el "el stack
// nunca cayo" (que se sabe leyendo el boot_time) del "ya paso suficiente tiempo"
// (que era el reloj que se elimina).
bool state_stack_never_stopped(void) {
    if (g_state != SERVER_STATE_ON) return false;
    if (!g_boot_verified_live || !g_boot_confirmed_for_on) return false;
    // g_boot_changed_ms == 0 -> el boot_time leido en vivo es el mismo que
    // teniamos: nadie arranco de cero, el stack sigue en pie desde antes.
    return g_boot_changed_ms == 0;
}

uint32_t state_last_poll_ms(void) { return g_last_poll_ms; }
bool state_last_poll_tcp_alive(void) { return g_last_poll_tcp_alive; }

uint32_t state_server_uptime_s(void) {
    uint32_t bt = g_server_boot_time;
    if (bt == 0) return 0;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint32_t now = (uint32_t)tv.tv_sec;
    return (now > bt) ? (now - bt) : 0;
}

const char *server_state_str(server_state_t s) {
    switch (s) {
        case SERVER_STATE_OFF:     return "OFF";
        case SERVER_STATE_SUSPEND: return "SUSPEND";
        case SERVER_STATE_ON:      return "ON";
        default:                     return "UNKNOWN";
    }
}

server_state_t state_get(void) { return g_state; }
uint32_t state_entered_at_ms(void) { return g_state_entered_ms; }
uint32_t state_last_known_on_ms(void) { return g_last_known_on_ms; }

void state_set(server_state_t s) {
    if (s == g_state) return;
    // Una vez OFF, solo ON puede sacarnos de ahí. Esto evita que el poll
    // reporte SUSPEND transitorio durante el shutdown y reabra el ciclo
    // de auto-off.
    if (g_state == SERVER_STATE_OFF && s != SERVER_STATE_ON) {
        ESP_LOGD(TAG, "ignoring transition OFF -> %s (would re-trigger automation)",
                 server_state_str(s));
        return;
    }
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    ESP_LOGI(TAG, "state %s -> %s",
             server_state_str(g_state), server_state_str(s));
    server_state_t prev = g_state;
    g_state = s;
    g_state_entered_ms = now;
    if (s == SERVER_STATE_ON) {
        g_last_known_on_ms = now;
    } else if (s == SERVER_STATE_OFF) {
        // Al apagar explícitamente a OFF (o por /off), reseteamos
        // g_last_known_on_ms para que la heurística de query_state
        // NUNCA degrade un equipo apagado a SUSPEND (S3).
        g_last_known_on_ms = 0;
        // El boot_time cacheado queda obsoleto: el próximo arranque (en frio)
        // debe verse como boot NUEVO para que la espera del stack web aplique,
        // mientras que un simple resume desde S3 no debe pagarla.
        g_boot_changed_ms = 0;
        g_boot_verified_live = false;
    }
    // Al salir de ON el boot_time cacheado deja de ser confiable: si el server
    // vuelve, hay que releer /state antes de decidir si fue arranque en frio.
    // Ademas se limpia la marca de "arranque en frio": sin esto, un resume
    // desde S3 posterior a un arranque en frio seguia pagando la espera de
    // 45 s (el timestamp viejo hacia que todo ON posterior se creyera frio).
    if (s != SERVER_STATE_ON) {
        g_boot_confirmed_for_on = false;
        g_boot_changed_ms = 0;
        g_cold_boot_since_on_ms = 0;
    }
    // Cualquier transición a ON (OFF->ON, SUSPEND->ON, UNKNOWN->ON) implica
    // que alguien prendió o despertó el servidor → resetear timer de inactividad.
    if (s == SERVER_STATE_ON && prev != SERVER_STATE_ON) {
        automation_kick();
    }

    // Rastro de suspensión:
    //  - ON  -> el equipo volvio: ya no hay nada que despertar.
    //  - SUSPEND -> lo vemos (o lo mandamos) dormir: hay que despertarlo con WOL
    //    antes de poder apagarlo.
    //  - OFF desde SUSPEND -> es la degradacion del poll (5 min sin respuesta):
    //    el equipo sigue en S3, el rastro se mantiene.
    //  - OFF desde ON/UNKNOWN -> apagado real: no queda rastro.
    if (s == SERVER_STATE_ON) {
        set_suspect_suspended(false);
    } else if (s == SERVER_STATE_SUSPEND) {
        set_suspect_suspended(true);
    } else if (s == SERVER_STATE_OFF) {
        if (prev == SERVER_STATE_SUSPEND) {
            // Degradacion del poll: el equipo sigue en S3, el rastro se mantiene.
            set_suspect_suspended(true);
        } else if (prev == SERVER_STATE_ON) {
            // Estaba encendido y se apago de verdad -> no hay nada que despertar.
            set_suspect_suspended(false);
        }
        // prev == UNKNOWN: se conserva el valor restaurado de NVS (puede venir
        // de un boot anterior donde el servidor quedo suspendido).
    }
}

void state_mark_known_on(void) {
    // Si ya está confirmado ON, nada que hacer
    if (g_state == SERVER_STATE_ON) return;
    // NO forzar g_state = ON ni g_last_known_on_ms = now antes de que
    // el servidor responda TCP: hacerlo rompía la protección OFF->SUSPEND
    // si el boot tarda más de lo que tarda el siguiente poll en correr.
    // Solo kickeamos automation para reiniciar el reloj de inactividad.
    automation_kick();
}

// GET /state a server via BSD sockets. Devuelve >=0 con bytes escritos en
// buf (el body ya extraido, sin headers HTTP), o <0 en error/timeout.
// Usado tanto por query_http_state (parsea el campo "state") como por
// refresh_boot_time (parsea el campo "boot_time").
//
// IMPORTANTE: en lwIP (IDF v5.3), connect() BLOQUEANTE no respeta los
// timeouts SO_SNDTIMEO/SO_RCVTIMEO del socket. Si la IP destino no
// responde ARP/SYN (caso típico: server apagado), connect() puede
// quedarse colgado minutos retransmitiendo. Por eso:
//   1) creamos el socket
//   2) lo pasamos a O_NONBLOCK
//   3) hacemos connect() no-bloqueante (devuelve -1 con errno=EINPROGRESS)
//   4) esperamos con select() + timeout duro de 2s
//   5) chequeamos SO_ERROR para detectar fallos reales (ECONNREFUSED, etc.)
static int server_http_get_body(char *buf, int buf_size) {
    if (buf == NULL || buf_size <= 16) return -1;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    // O_NONBLOCK para que connect() retorne EINPROGRESS inmediatamente.
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags == -1 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) == -1) {
        close(sock);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(CONFIG_SERVER_HTTP_PORT);
    addr.sin_addr.s_addr = inet_addr(CONFIG_SERVER_IP);

    int rc = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        ESP_LOGD(TAG, "[STATE] connect() immediate fail errno=%d", errno);
        close(sock);
        return -1;
    }
    if (rc < 0) {
        // Esperar con select() hasta 2s a que termine el handshake TCP.
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
        int sr = select(sock + 1, NULL, &wfds, NULL, &tv);
        if (sr <= 0) {
            ESP_LOGD(TAG, "[STATE] connect() timeout (sr=%d errno=%d)", sr, errno);
            close(sock);
            return -1;
        }
        // Chequear si el handshake fallo (RST, ARP sin respuesta, etc.)
        int sockerr = 0;
        socklen_t slen = sizeof(sockerr);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &sockerr, &slen) < 0 || sockerr != 0) {
            ESP_LOGD(TAG, "[STATE] connect() failed sockerr=%d", sockerr);
            close(sock);
            return -1;
        }
    }
    // A partir de aca el socket esta conectado. Restauramos modo bloqueante
    // y ponemos timeouts de send/recv por si el server cuelga.
    fcntl(sock, F_SETFL, flags);
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    char req[384];
    int req_len = snprintf(req, sizeof(req),
        "GET /state HTTP/1.1\r\n"
        "Host: %s\r\n"
        "X-ESP32-Secret: %s\r\n"
        "Connection: close\r\n"
        "User-Agent: esp32-state-poll/1.0\r\n"
        "\r\n",
        CONFIG_SERVER_IP, CONFIG_SERVER_SHUTDOWN_SECRET);
    if (req_len <= 0 || req_len >= (int)sizeof(req)) {
        close(sock);
        return -1;
    }

    int sent = 0;
    while (sent < req_len) {
        int n = send(sock, req + sent, req_len - sent, 0);
        if (n <= 0) { close(sock); return -1; }
        sent += n;
    }

    // Leer hasta encontrar fin de headers (\r\n\r\n) o agotar el buffer.
    int total = 0;
    int header_end = -1;
    while (total < buf_size - 1) {
        int n = recv(sock, buf + total, buf_size - 1 - total, 0);
        if (n < 0) { close(sock); return -1; }
        if (n == 0) break;
        total += n;
        buf[total] = 0;
        char *p = strstr(buf, "\r\n\r\n");
        if (p != NULL) {
            header_end = (p - buf) + 4;
            break;
        }
    }
    close(sock);

    if (header_end < 0 || total == 0) return -1;

    int status_code = 0;
    if (sscanf(buf, "HTTP/1.%*d %d", &status_code) != 1 || status_code != 200) {
        return -1;
    }

    int body_len = total - header_end;
    if (body_len <= 0) return 0;
    memmove(buf, buf + header_end, body_len);
    buf[body_len] = 0;
    return body_len;
}

static void refresh_boot_time(void) {
    char buf[512];
    s_state_http_attempts++;
    int body_len = server_http_get_body(buf, sizeof(buf));
    s_state_http_last_len = body_len;
    if (body_len <= 0) {
        s_state_http_err++;
        ESP_LOGW(TAG, "refresh_boot_time: /state ilegible desde server (body_len=%d)",
                 body_len);
        return;
    }
    s_state_http_ok++;
    char *js = strchr(buf, '{');
    if (js == NULL) {
        ESP_LOGW(TAG, "refresh_boot_time: /state sin JSON (%.60s)", buf);
        return;
    }
    cJSON *doc = cJSON_Parse(js);
    if (doc == NULL) {
        ESP_LOGW(TAG, "refresh_boot_time: /state JSON invalido (%.60s)", js);
        return;
    }
    const cJSON *bt = cJSON_GetObjectItemCaseSensitive(doc, "boot_time");
    if (cJSON_IsNumber(bt)) {
        note_boot_time_live((uint32_t)bt->valuedouble);
    } else {
        ESP_LOGW(TAG, "refresh_boot_time: /state sin campo boot_time (%.60s)", js);
    }
    const cJSON *act = cJSON_GetObjectItemCaseSensitive(doc, "active");
    if (act != NULL) {
        bool is_act = (cJSON_IsBool(act) && cJSON_IsTrue(act)) ||
                      (cJSON_IsNumber(act) && act->valueint != 0);
        if (is_act) {
            ESP_LOGI(TAG, "server reports user activity (kernel packet/socket counter) -> automation_kick");
            automation_kick();
        }
    }
    cJSON_Delete(doc);
}

// Implementacion con BSD sockets directos via el helper server_http_get_body.
// ANTES: usaba esp_http_client (con perform() u open+fetch_headers), pero
// en IDF v5.3 el primer poll tras un RESET se colgaba 60-80 segundos
// antes de retornar error (logs `esp-tls: [sock=60] select() timeout`).
// Causa: el SSL transport se inicializa siempre en esp_http_client_init()
// (no solo para https) y esp_tls_plain_tcp_connect() reusa timeouts del
// contexto SSL anterior, lo que rompe el timeout_ms de 2s configurado en
// config. Como es HTTP plano, no necesitamos TLS ni la capa de abstraccion
// de esp_http_client: BSD sockets + select() son predecibles y baratos.
static const char *query_http_state(void) {
    static char result_buf[16];
    result_buf[0] = 0;

    char buf[512];
    int body_len = server_http_get_body(buf, sizeof(buf));
    if (body_len < 0) {
        // connect falló o timeout o status != 200
        ESP_LOGD(TAG, "[STATE] HTTP get failed (no body, treating as TIMEOUT)");
        strncpy(result_buf, "TIMEOUT", sizeof(result_buf) - 1);
        return result_buf;
    }
    if (body_len == 0) {
        strncpy(result_buf, "ERR_NOBODY", sizeof(result_buf) - 1);
        return result_buf;
    }

    char *json_start = strchr(buf, '{');
    if (json_start == NULL) {
        strncpy(result_buf, "ERR_NOJSON", sizeof(result_buf) - 1);
        return result_buf;
    }

    cJSON *doc = cJSON_Parse(json_start);
    if (doc == NULL) {
        ESP_LOGD(TAG, "[STATE] JSON parse error: %.60s", json_start);
        strncpy(result_buf, "ERR_PARSE", sizeof(result_buf) - 1);
        return result_buf;
    }

    const cJSON *state_node = cJSON_GetObjectItemCaseSensitive(doc, "state");
    if (cJSON_IsString(state_node) && state_node->valuestring != NULL) {
        strncpy(result_buf, state_node->valuestring, sizeof(result_buf) - 1);
        result_buf[sizeof(result_buf) - 1] = 0;
    } else {
        strncpy(result_buf, "ERR_NOSTATE", sizeof(result_buf) - 1);
    }
    const cJSON *bt_node = cJSON_GetObjectItemCaseSensitive(doc, "boot_time");
    if (cJSON_IsNumber(bt_node)) {
        note_boot_time_live((uint32_t)bt_node->valuedouble);
    }
    const cJSON *act_node = cJSON_GetObjectItemCaseSensitive(doc, "active");
    if (act_node != NULL) {
        bool is_act = (cJSON_IsBool(act_node) && cJSON_IsTrue(act_node)) ||
                      (cJSON_IsNumber(act_node) && act_node->valueint != 0);
        if (is_act) {
            ESP_LOGI(TAG, "server reports user activity (kernel packet/socket counter) -> automation_kick");
            automation_kick();
        }
    }
    cJSON_Delete(doc);
    return result_buf;
}

// Resultado consolidado del poll: lo unico que query_state decide es el
// estado final a partir de la combinacion TCP + HTTP + tiempo desde el
// ultimo ON conocido. Antes tenia un caso "NOCONN" que nunca se retornaba
// (codigo muerto). Tambien tenia el problema de que cualquier resultado
// ERR_* (no-ON, no-OFF, no-TIMEOUT) caia a UNKNOWN indefinidamente, lo
// que dejaba el estado en UNKNOWN tras un RESET si el primer HTTP poll
// fallaba por timeout. Ahora: cualquier fallo HTTP -> TIMEOUT-equivalente,
// y la heuristica LAST_ON_HEURISTIC_MS decide OFF vs SUSPEND.
static server_state_t query_state(void) {
    g_last_poll_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (server_tcp_alive(400)) {
        g_last_poll_tcp_alive = true;
        // IMPORTANTE: refrescar "lo vi vivo" en CADA poll, no solo cuando cambia
        // el estado. Antes g_last_known_on_ms solo se actualizaba en las
        // transiciones a ON, asi que un equipo que llevaba horas encendido y
        // luego se suspendia (S3) quedaba con un "lo vi vivo" de hace horas:
        // la heuristica de 5 min lo degradaba a OFF en vez de SUSPEND, y el
        // /off contestaba "already off" sin apagar nada.
        g_last_known_on_ms = g_last_poll_ms;
        note_alive_now();
        return SERVER_STATE_ON;
    }
    g_last_poll_tcp_alive = false;

    const char *http_result = query_http_state();

    if (strcmp(http_result, "ON") == 0) {
        g_last_known_on_ms = g_last_poll_ms;
        note_alive_now();
        return SERVER_STATE_ON;
    }
    if (strcmp(http_result, "OFF") == 0) return SERVER_STATE_OFF;

    // Cualquier fallo (TIMEOUT, ERR_*, etc.) se trata igual: server no
    // responde. La decision OFF vs SUSPEND la da LAST_ON_HEURISTIC_MS:
    // - si nunca vimos ON o hace >5min -> OFF (realmente apagado)
    // - si hace <5min -> SUSPEND (puede estar durmiendo S3)
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (http_result != NULL && strncmp(http_result, "ERR_", 4) != 0 &&
        g_state != SERVER_STATE_OFF &&
        seen_alive_recently()) {
        ESP_LOGW(TAG, "[STATE] poll returned '%s' but recently ON, treating as SUSPEND",
                 http_result);
        return SERVER_STATE_SUSPEND;
    }
    // Fallo limpio (TIMEOUT) o ERR_* -> OFF. Esto elimina UNKNOWN como
    // estado permanente: si el poll falla por cualquier motivo, marcamos
    // OFF y dejamos que el siguiente poll (30s) revalide.
    ESP_LOGD(TAG, "[STATE] poll result='%s', treating as OFF (last_on=%u ms ago)",
             http_result ? http_result : "?",
             (g_last_known_on_ms != 0) ? (unsigned)(now - g_last_known_on_ms) : 0);
    return SERVER_STATE_OFF;
}

static TaskHandle_t s_state_task = NULL;
static volatile uint32_t s_fast_poll_until_ms = 0;

void state_trigger_fast_poll(void) {
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    s_fast_poll_until_ms = now + (60UL * 1000UL); // 60 segundos de fast poll
    if (s_state_task != NULL) {
        xTaskNotifyGive(s_state_task);
    }
}

bool state_is_fast_polling(void) {
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    return (now < s_fast_poll_until_ms) && (g_state != SERVER_STATE_ON);
}

void state_poll_task(void *arg) {
    s_state_task = xTaskGetCurrentTaskHandle();

    // First poll after a short delay so we don't block app_main() with HTTP I/O
    // (which would trip the task watchdog).
    vTaskDelay(pdMS_TO_TICKS(2000));
    server_state_t new_state = query_state();
    if (new_state != SERVER_STATE_UNKNOWN) {
        state_set(new_state);
    }
    // boot_time solo importa cuando el server está ON; si está OFF/SUSPEND no hace falta HTTP
    if (g_state == SERVER_STATE_ON || g_server_boot_time == 0) {
        refresh_boot_time();
    }
    while (1) {
        system_health_heartbeat(HEALTH_CHANNEL_STATE);

        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        bool fast = (now < s_fast_poll_until_ms) && (g_state != SERVER_STATE_ON);
        uint32_t delay_ms = fast ? 2500 : (CONFIG_SERVER_POLL_INTERVAL_S * 1000);

        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(delay_ms));

        system_health_heartbeat(HEALTH_CHANNEL_STATE);
        server_state_t new_state = query_state();
        if (new_state != SERVER_STATE_UNKNOWN) {
            state_set(new_state);
        }
        if (g_state == SERVER_STATE_ON || g_server_boot_time == 0) {
            refresh_boot_time();
        }
    }
}

void state_init(void) {
    ESP_LOGI(TAG, "state init (lazy — first poll in poll task)");
    g_state = SERVER_STATE_UNKNOWN;
    g_state_entered_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    g_last_known_on_ms = 0;
    g_last_poll_ms = 0;
    g_last_poll_tcp_alive = false;
    g_server_boot_time = load_persisted_boot_time();
    // Ultimo instante en que vimos el servidor con vida (puede ser de un
    // boot anterior): sin esto, tras el auto-reboot horario el ESP32
    // "olvidaba" que el servidor estaba suspendido en S3.
    {
        nvs_handle_t h;
        uint32_t v = 0;
        if (nvs_open(NVS_NAMESPACE_STATE, NVS_READONLY, &h) == ESP_OK) {
            if (nvs_get_u32(h, NVS_KEY_LAST_ALIVE, &v) == ESP_OK) {
                g_last_alive_unix = v;
            }
            nvs_close(h);
        }
        if (g_last_alive_unix != 0) {
            ESP_LOGI(TAG, "restaurado last_alive_unix=%u", (unsigned)g_last_alive_unix);
        }
    }
    // El rastro de suspension sobrevive al auto-reboot: sin el, un /off
    // despues de un reinicio del ESP32 no distinguiria "apagado" de "suspendido".
    {
        nvs_handle_t h;
        uint8_t v = 0;
        if (nvs_open(NVS_NAMESPACE_STATE, NVS_READONLY, &h) == ESP_OK) {
            if (nvs_get_u8(h, NVS_KEY_SUSPECT_SUSP, &v) == ESP_OK) {
                g_suspect_suspended = (v != 0);
            }
            nvs_close(h);
        }
        if (g_suspect_suspended) {
            ESP_LOGI(TAG, "rastro de suspension restaurado de NVS: 1");
        }
    }
    // El boot_time de NVS es orientativo hasta que hagamos una lectura en vivo
    // de /state en este boot: recien ahi sabemos si el server arranco de cero.
    g_boot_changed_ms = 0;
    g_boot_verified_live = false;
    g_boot_confirmed_for_on = false;
    g_cold_boot_since_on_ms = 0;
    if (g_server_boot_time != 0) {
        ESP_LOGI(TAG, "loaded persisted boot_time: %u", (unsigned)g_server_boot_time);
    }
    // No blocking HTTP I/O here. state_poll_task does the first poll 2s after
    // it starts; app_main() returns immediately so the TWDT doesn't fire.
    ESP_LOGI(TAG, "initial state: %s", server_state_str(g_state));
}