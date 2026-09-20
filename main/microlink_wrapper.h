#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>

#include "microlink.h"

esp_err_t microlink_wrapper_start(void);
esp_err_t microlink_wrapper_stop(void);
bool microlink_wrapper_is_connected(void);
uint32_t microlink_wrapper_get_vpn_ip(void);

/* Estado actual del stack MicroLink. Lectura segura desde cualquier task
 * (el campo es volatile en el handle). */
microlink_state_t microlink_wrapper_get_state(void);

/* Tick (ms) del ultimo ML_STATE_CONNECTED observado por el callback.
 * 0 = nunca se vio CONNECTED todavia. Usado por el watchdog para
 * medir tiempo en estado caido. Lectura atomica. */
uint32_t microlink_wrapper_get_last_connected_ms(void);

/* Milisegundos desde el ultimo byte REALMENTE RECIBIDO del control plane
 * (base de tiempo esp_timer del propio microlink; 0 = nunca recibido).
 * Se diferencia de get_state() y de is_connected(): mientras la sesion
 * Tailscale este muerta en silencio (TCP half-open tras caida del enlace,
 * los H2 PING siguen "enviandose" al buffer local pero nada vuelve),
 * el state permanece CONNECTED pero esto crece sin limite. Es la senal
 * de vida que el watchdog externo necesita. */
uint32_t microlink_wrapper_get_ctrl_stale_ms(void);

/* Nombre legible de un microlink_state_t (string estatico). */
const char *microlink_wrapper_state_name(microlink_state_t s);

/* Lightweight recovery: reabre sockets en la interfaz de red actual sin
 * destruir la sesion Tailscale. Preserva keys, peers, VPN IP. Usado solo
 * para diagnostico manual (serial "tailscale rebind"); el watchdog
 * autonomo ya no lo invoca porque stop+start de MicroLink tiene un bug
 * upstream que deja la sesion colgada en WIFI_WAIT.
 * Retorna ESP_ERR_INVALID_STATE si Tailscale esta apagado. */
esp_err_t microlink_wrapper_rebind(void);

/* On-demand WG handshake trigger for a peer.
 * Sends handshake via DERP (always) + direct-UDP (if endpoint known).
 * The direct-UDP path is critical to wake lazy peers (see ml_wg_mgr.c). */
esp_err_t microlink_wrapper_trigger_handshake(uint32_t dest_vpn_ip);

/* Send DISCO CallMeMaybe to a peer — tells them our endpoints
 * (LAN IP, STUN public IP) so they can initiate a WG handshake
 * to us. */
esp_err_t microlink_wrapper_send_cmm(uint32_t dest_vpn_ip);

/* Check if the WG tunnel to a peer has an active session. */
bool microlink_wrapper_peer_is_up(uint32_t dest_vpn_ip);
/* Self node MagicDNS FQDN (trailing dot stripped), e.g.
 * "esp32-server.your-tailnet.ts.net". NULL until Tailscale is CONNECTED. */
const char *microlink_wrapper_get_fqdn(void);

/* Publish a TXT record via the control plane (ACME DNS-01 bridge).
 * Blocks up to timeout_ms; ESP_OK when the control accepted it. */
esp_err_t microlink_wrapper_set_dns(const char *name, const char *value,
                                    uint32_t timeout_ms);
