#pragma once

#include <esp_err.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Mapeo programable de puertos: ESP32 escucha en `local_port` y redirige
// (HTTP 302) o forwardea (TCP crudo) hacia `upstream_host:upstream_port`.
//
// Cada entrada es independiente: una conexión al :8080 va a un host:puerto,
// una al :8443 a otro. `proto` define qué aparece en el header `Location:`
// del 302 ("http" o "https"). `label` es solo cosmético para /portmap list.
//
// `upstream_host` vacío = auto-routing según IP del peer:
//   - peer en LAN (192.168.x.x) -> CONFIG_COCOPA01_IP
//   - peer por Tailscale u otro -> CONFIG_COCOPA01_TS_IP
// Host explícito (IP o hostname) = se usa tal cual. Soporta Tailscale IPs,
// hostnames de cloudflared tunnels, etc. DNS lookup se hace en el handler
// (no en el listener, para no bloquear el accept loop).
//
// Persistencia: NVS namespace "portmap", un único string JSON con el array
// completo. Mutex protege s_entries + s_listeners.

#define PORTMAP_MAX_ENTRIES    8
#define PORTMAP_HOST_MAX       64
#define PORTMAP_PROTO_MAX      8
#define PORTMAP_LABEL_MAX      24

typedef struct {
    uint16_t local_port;
    char     upstream_host[PORTMAP_HOST_MAX];
    uint16_t upstream_port;
    char     proto[PORTMAP_PROTO_MAX];
    char     label[PORTMAP_LABEL_MAX];
} portmap_entry_t;

#define PORTMAP_STATUS_BUF_SIZE 768

// Carga config desde NVS (o siembra defaults si está vacío), arranca los
// listeners. Llamar después de wifi_init_sta y state_init.
esp_err_t portmap_init(void);

// Agrega un mapping. Persiste en NVS y arranca un listener nuevo. Atómico:
// si el bind falla, no se guarda.
//   local_port    1024..65535 (rechaza privileged)
//   upstream_host "" o "ip" o "hostname" ("" = auto-routing LAN/TS)
//   upstream_port 1..65535
//   proto         "http" o "https" (default "http")
//   label         texto libre (opcional)
esp_err_t portmap_add(uint16_t local_port,
                      const char *upstream_host,
                      uint16_t upstream_port,
                      const char *proto,
                      const char *label);

// Quita el mapping cuyo local_port coincide. Para el listener, libera y
// persiste. Retorna ESP_ERR_NOT_FOUND si no existe.
esp_err_t portmap_del(uint16_t local_port);

// Vuelve a los defaults de compilación: para todos los listeners, siembra
// [8080, "", 8080, "http", "cocopa"], persiste y rearranca.
esp_err_t portmap_reset_defaults(void);

// Cantidad actual de mappings activos.
int portmap_count(void);

// Copia hasta `max_entries` entradas en `out`. Retorna la cantidad en *count.
esp_err_t portmap_get_all(portmap_entry_t *out, int max_entries, int *count);

// Llena `out` con la tabla multi-línea lista para enviar por Telegram.
// `out` debe apuntar a un buffer de al menos PORTMAP_STATUS_BUF_SIZE bytes.
void portmap_format_status(char *out, size_t out_size);
