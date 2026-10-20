#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Adaptador (no puro) de la deteccion de cortes: la task que decide, la
// persistencia en NVS y los avisos por Telegram. La maquina de estados vive en
// power_event_logic.c (testeable en host).

// arranca la task que vigila el estado del corte y ejecuta la escalada.
esp_err_t power_event_init(void);

// El ESP32 perdio la red (lo llama wifi.c en STA_DISCONNECTED).
void power_event_note_net_down(void);
// El ESP32 recupero la red (lo llama wifi.c en GOT_IP).
void power_event_note_net_up(void);
// El servidor volvio a responder (lo llama state.c al transicionar a ON).
void power_event_note_server_on(void);
// Nosotros apagamos el servidor a proposito (lo llama power_off.c).
void power_event_note_intentional_off(void);

// Estado legible para /diag.
void power_event_status_str(char *out, size_t out_size);

// Diagnostico: simula una caida de red de N segundos (sin tocar el WiFi de
// verdad) para poder probar la deteccion de cortes y la escalada sin tener
// que cortar la luz.
void power_event_simulate_outage(uint32_t seconds);
