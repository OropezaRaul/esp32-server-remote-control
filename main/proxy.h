#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "portmap.h"

// TCP proxy + HTTP redirect. Antes escuchaba en un solo puerto hardcodeado
// (8080). Ahora escucha en N puertos, uno por cada entrada activa de
// `portmap`. Para cada conexión:
//
//   - Si llega un request HTTP -> 302 a <proto>://<upstream_host>:<upstream_port><path>
//   - Si llega TCP crudo      -> forward bidireccional
//
// Antes de redirigir/forwardear, `ensure_upstream_up()` se asegura de que
// el destino esté respondiendo. Si no, manda WOL y espera (con reintentos)
// hasta `PROXY_WAKE_TIMEOUT_MS`. Esto cubre el caso S5 -> S0 del server.
//
// `automation_kick()` se llama en cada conexión: actividad de cliente es
// señal de "sistema en uso" y resetea el timer de auto-suspend/auto-off.

#define PROXY_HTTP_PROBE_MS     2000
#define PROXY_FWD_TIMEOUT_S     300
#define PROXY_WAKE_TIMEOUT_MS   90000
#define PROXY_WAKE_POLL_MS      2000
#define PROXY_LISTEN_BACKLOG    4
#define PROXY_TASK_STACK        16384
#define PROXY_TASK_PRIO         5
#define PROXY_LIS_TASK_STACK    8192
#define PROXY_LIS_TASK_PRIO     6

// Struct público (no opaco) para que portmap.c pueda inspeccionar el
// local_port al buscar el listener que corresponde a una entry. Los campos
// internos (sock fd, task handle, stopping flag) son detalles de proxy.c
// y no deberían ser modificados por nadie más.
typedef struct proxy_listener_ctx {
    portmap_entry_t entry;
    int             listen_sock;
    TaskHandle_t    task;
    volatile bool   stopping;
} proxy_listener_ctx_t;

// Crea un socket en <entry->local_port> y arranca la task listener que
// acepta conexiones y las dispatcha a handle_connection. Retorna NULL si
// falla el bind o la creación de la task. La entry se copia por valor
// dentro del ctx, así que el caller puede liberar su copia.
proxy_listener_ctx_t *proxy_listener_create(const portmap_entry_t *entry);

// Señaliza stop, cierra el socket, espera a que la task termine y libera.
// Seguro llamarlo aunque la task ya haya salido por error de socket.
void proxy_listener_destroy(proxy_listener_ctx_t *ctx);
