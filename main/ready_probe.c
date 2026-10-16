#include "ready_probe.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>

#include "cocopa01_client.h"
#include "ready_logic.h"
#include "state.h"

#include "sdkconfig.h"

static const char *TAG = "ready";



#define READY_TICK_MS        500UL
#define READY_TUNNEL_TCP_MS  400    
#define READY_HTTP_MS       1200    
#define READY_API_MS         1500    


#define READY_FUNNEL_RECHECK_EVERY 300U





static ready_logic_t s_logic;
static TaskHandle_t s_task = NULL;
static char s_status[96] = "ready: inactivo";
static char s_host[96] = CONFIG_COCOPA01_TS_HOSTNAME;
static char s_path[96] = "/cocopa/";



static char s_api_path[96] = "/api/v1/health";





static bool s_funnel_reachable = false;
static bool s_funnel_warned = false;


static uint32_t s_off_observations = 0;

static uint32_t s_probes_since_funnel_check = 0;

static uint32_t now_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}





static void derive_target(void) {
    const char *url = CONFIG_GATE_COCOPA_URL;
    if (url == NULL || *url == 0) return;
    const char *p = strstr(url, "://");
    p = p ? (p + 3) : url;
    size_t i = 0;
    while (*p && *p != '/' && i < sizeof(s_host) - 1) s_host[i++] = *p++;
    s_host[i] = 0;
    if (*p == '/' && i < sizeof(s_path) - 1) {
        snprintf(s_path, sizeof(s_path), "%s", p);
    }
    // La sonda pide el path con barra final: sin ella nginx responde 301 hacia
    // la version con barra, que es evidencia mas debil que un 200 real.
    if (s_path[0] != 0 && s_path[strlen(s_path) - 1] != '/') {
        size_t n = strlen(s_path);
        if (n + 1 < sizeof(s_path)) { s_path[n] = '/'; s_path[n + 1] = 0; }
    }
    // El health del backend cuelga del mismo prefijo que el shell: /cocopa ->
    // /cocopa/api/v1/health.
    {
        char base[80];
        snprintf(base, sizeof(base), "%s", s_path);
        size_t n = strlen(base);
        while (n > 0 && base[n - 1] == '/') base[--n] = 0;
        snprintf(s_api_path, sizeof(s_api_path), "%s/api/v1/health", base);
    }
    ESP_LOGI(TAG, "sonda: GET %s:%d%s y %s (Host %s)", CONFIG_COCOPA01_IP,
             (unsigned)CONFIG_COCOPA01_PROXY_PORT, s_path, s_api_path, s_host);
}

static void set_status_text(const char *text) {
    snprintf(s_status, sizeof(s_status), "%s", text);
}



static void probe_once(void) {
    uint32_t t0 = now_ms();
    bool was_ready = ready_logic_is_ready(&s_logic, t0, READY_LOGIC_TTL_MS);

    // 1) EVIDENCIA OBLIGATORIA: la peticion que el cliente va a hacer, resuelta
    //    por la IP LAN con el Host publico (mismo virtual host de nginx que el
    //    tunel selecciona). Un 502 explicito cuenta como todavia NO.
    int32_t status = cocopa01_http_probe_at(CONFIG_COCOPA01_IP,
                                           CONFIG_COCOPA01_PROXY_PORT,
                                           s_host, s_path, READY_HTTP_MS);
    bool backend = (status >= 200 && status < 400);

    // 1b) Y ahora la parte que de verdad importa para el operador: el BACKEND.
    //     nginx levanta el shell de la SPA antes de que FastAPI termine el startup,
    //     asi que hay una ventana (medida: varios segundos en arranque en frio) en la
    //     que /cocopa/ responde 200 pero /cocopa/api/v1/ todavia no tiene upstream y
    //     responde 502. En esa ventana la SPA abria con "No hay usuarios registrados"
    //     y un boton de crear admin, y el operador tenia que recargar a mano.
    //     Con esta segunda comprobacion el latch no se enciende hasta que la API
    //     responde de verdad.
    int32_t api_status = -1;
    if (backend) {
        api_status = cocopa01_http_probe_at(CONFIG_COCOPA01_IP, CONFIG_COCOPA01_PROXY_PORT,
                                             s_host, s_api_path, READY_API_MS);
    }
    bool api_ok = (api_status >= 200 && api_status < 400);
    s_logic.last_api_status = api_status;

    // 2) EVIDENCIA COMPLEMENTARIA: tailscaled escuchando el Funnel en :443. Si el
    //    ESP32 alcanza ese puerto, un "no" bloquea (el tunel daria 502). Si no hay
    //    ruta tailnet desde el ESP32, el chequeo se omite porque no es informacion
    //    que el ESP32 pueda tener: quien verifica la ruta publica de punta a
    //    punta es cocopa01, con GET /notify/ready.
    //
    // OJO con el costo: cuando esa ruta NO existe (medido el 2026-10-15), cada
    // connect se comia los 400 ms completos del timeout en cada sonda, o sea dos
    // veces por segundo de espera tonta que ademas retrasaba la deteccion. Por eso
    // se hace una vez y, si falla, solo se reintenta de vez en cuando: si alguna
    // vez se logra, a partir de ahi se exige en cada sonda (1-3 ms, son peers).
    bool tunnel = s_funnel_reachable;
    if (s_funnel_reachable || s_probes_since_funnel_check >= READY_FUNNEL_RECHECK_EVERY) {
        tunnel = cocopa01_tcp_port_alive_at(CONFIG_COCOPA01_TS_IP,
                                            CONFIG_COCOPA01_FUNNEL_PORT,
                                            READY_TUNNEL_TCP_MS);
        s_probes_since_funnel_check = 0;
    } else {
        s_probes_since_funnel_check++;
    }
    if (tunnel) {
        s_funnel_reachable = true;
    } else if (backend && !s_funnel_reachable && !s_funnel_warned) {
        s_funnel_warned = true;
        ESP_LOGW(TAG, "sin ruta tailnet del ESP32 a %s:%d: el chequeo del listener del Funnel queda informativo y solo se reintenta cada %u sondas; la verificacion de punta a punta la hace cocopa01 con GET /notify/ready",
                 CONFIG_COCOPA01_TS_IP, (unsigned)CONFIG_COCOPA01_FUNNEL_PORT,
                 (unsigned)READY_FUNNEL_RECHECK_EVERY);
    }

    bool ok = backend && api_ok && (tunnel || !s_funnel_reachable);
    uint32_t done = now_ms();
    ready_logic_record(&s_logic, done, ok, status, done - t0);

    char line[96];
    ready_logic_format(&s_logic, done, line, sizeof(line));
    set_status_text(line);

    if (ok && !was_ready) {
        ESP_LOGI(TAG, "cocopa01 LISTO: SPA HTTP %ld + API HTTP %ld en %ums%s", (long)status, (long)api_status, (unsigned)(done - t0), tunnel ? " + funnel :443 ok" : " (funnel no verificado desde el ESP32)");
        ESP_LOGI(TAG, "  -> %s", line);
    } else if (!ok && was_ready) {
        ESP_LOGW(TAG, "cocopa01 dejo de responder (backend HTTP %ld): el latch cae", (long)status);
    } else if (!ok) {
        ESP_LOGD(TAG, "aun no: SPA HTTP %ld, API HTTP %ld (%ums)", (long)status, (long)api_status, (unsigned)(done - t0));
    }
}

static void ready_task(void *arg) {
    ESP_LOGI(TAG, "sonda de readiness lista: 1s con gente esperando, %us en mantenimiento, TTL %us",
             (unsigned)(READY_LOGIC_SLOW_MS / 1000), (unsigned)(READY_LOGIC_TTL_MS / 1000));

    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(READY_TICK_MS));
        uint32_t now = now_ms();
        bool armed = ready_logic_armed(&s_logic, now);
        cocopa01_state_t st = state_get();

        if (st != COCOPA01_STATE_ON) {
            // Cocopa01 no esta: el latch viejo no puede seguir sirviendo de base
            // para un 302 (era exactamente la trampa de las sesiones 29 y 32).
            //
            // PERO con rebote de dos observaciones: durante un arranque el poll de
            // estado puede perder un connect() de 400 ms con la maquina saturada por
            // Docker y reportar OFF/SUSPEND un instante. Medido el 2026-10-15: eso
            // apagaba el latch y costaba un viaje extra del cliente (el splash)
            // cuando la app ya servia. El handler del 302 igual exige estado ON, asi
            // que un apagado real nunca redirige igual: este rebote solo evita el
            // parpadeo del latch, no cambia la logica de OFF ni la automation.
            if (++s_off_observations >= 2) {
                if (s_logic.latched) {
                    ESP_LOGI(TAG, "cocopa01 paso a %s: latch apagado",
                             cocopa01_state_str(st));
                    ready_logic_invalidate(&s_logic, now);
                }
            }
            // Nadie esperando -> no gastamos sondas. Con alguien esperando si
            // sondeamos: asi el latch se enciende apenas responde, aunque el poll
            // de estado todavia no haya marcado ON.
            if (!armed) continue;
        } else {
            s_off_observations = 0;
        }

        if (!ready_logic_due_probe(&s_logic, now, READY_LOGIC_FAST_MS,
                                   READY_LOGIC_SLOW_MS)) {
            continue;
        }
        probe_once();
    }
}

esp_err_t ready_init(void) {
    derive_target();
    ready_logic_init(&s_logic);
    BaseType_t ok = xTaskCreate(ready_task, "ready", 4096, NULL, 4, &s_task);
    if (ok != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "ready task create failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

void ready_arm(void) {
    if (s_task == NULL) return;
    ready_logic_arm(&s_logic, now_ms(), READY_LOGIC_ARM_MS);
    xTaskNotifyGive(s_task);
}

bool ready_is_ready(void) {
    return ready_logic_is_ready(&s_logic, now_ms(), READY_LOGIC_TTL_MS);
}

void ready_notify_ready(void) {
    uint32_t now = now_ms();
    if (!s_logic.latched) {
        ESP_LOGI(TAG, "cocopa01 avisa que ya esta listo (GET /notify/ready): el portal redirige de inmediato");
    }
    // ok=true con el codigo 0: el aviso del servidor es evidencia de por si solo,
    // no viene de una respuesta HTTP que el ESP32 haya hecho.
    ready_logic_set_public_broken(&s_logic, false);
    ready_logic_record(&s_logic, now, true, 0, 0);
    ready_logic_arm(&s_logic, now, READY_LOGIC_ARM_MS);
    if (s_task != NULL) xTaskNotifyGive(s_task);
    // Que el poll de estado se entere rapido de que el server esta arriba.
    state_trigger_fast_poll();
    char line[96];
    ready_logic_format(&s_logic, now, line, sizeof(line));
    set_status_text(line);
}

void ready_notify_unready(void) {
    uint32_t now = now_ms();
    ready_logic_set_public_broken(&s_logic, true);
    ready_logic_invalidate(&s_logic, now);

    set_status_text("ready: ruta publica de cocopa01 caida (aviso del servidor)");
}

bool ready_public_broken(void) {
    return s_logic.public_broken;
}

void ready_invalidate(void) {
    ready_logic_invalidate(&s_logic, now_ms());
}

void ready_get_status(ready_status_t *out) {
    if (out == NULL) return;
    uint32_t now = now_ms();
    memset(out, 0, sizeof(*out));
    out->latched = s_logic.latched;
    out->ready = ready_logic_is_ready(&s_logic, now, READY_LOGIC_TTL_MS);
    out->armed = ready_logic_armed(&s_logic, now);
    out->ready_ms_ago = s_logic.last_ok_ms ? (uint32_t)(now - s_logic.last_ok_ms) : 0;
    out->probes = s_logic.probes;
    out->ok = s_logic.ok;
    out->err = s_logic.err;
    out->last_duration_ms = s_logic.last_duration_ms;
    out->last_status = s_logic.last_status;
    out->last_api_status = s_logic.last_api_status;
}

const char *ready_status_text(void) {
    return s_status;
}
