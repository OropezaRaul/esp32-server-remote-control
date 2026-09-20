/*
 * ml_peerapi.h — Tailscale PeerAPI listener for MicroLink
 *
 * tailscaled always exposes a "PeerAPI" HTTP server reachable by other
 * tailnet nodes on the node's Tailscale IP at an ephemeral port, and
 * announces that port to control + peers via Hostinfo.Services:
 *   [{"Proto": "peerapi4", "Port": <port>}]
 *
 * MicroLink historically announced no Services, which is one observable
 * difference vs tailscaled when requesting Funnel (session 16: control
 * never granted the "funnel" nodecap despite IngressEnabled=true).
 * This listener closes that gap: it binds the announced port and serves
 * minimal PeerAPI responses. The /v0/ingress TCP-tunnel handler (needed
 * to actually receive Funnel traffic) plugs in here once the nodecap is
 * granted.
 *
 * Wire references (tailscale/tailscale main, 2026):
 *   - tailcfg.Service (Proto "peerapi4")      — tailcfg/tailcfg.go
 *   - peerAPI listener + /v0/ingress handler  — ipn/ipnlocal/peerapi.go,
 *                                               ipn/ipnlocal/serve.go
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the PeerAPI listener task.
 *
 * @param port  TCP port to listen on (bound to 0.0.0.0 so the WG netif
 *              receives traffic destined to the Tailscale IP).
 * @return ESP_OK if the listener task started (bind may still be retried
 *         internally if the interface is not up yet).
 */
esp_err_t ml_peerapi_start(uint16_t port);

/**
 * Stop the listener and free resources. Safe to call when not running.
 */
esp_err_t ml_peerapi_stop(void);

/**
 * @return the configured listener port, 0 if not started.
 */
uint16_t ml_peerapi_get_port(void);

#ifdef __cplusplus
}
#endif
