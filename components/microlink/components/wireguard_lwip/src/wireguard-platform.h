































#ifndef _WIREGUARD_PLATFORM_H_
#define _WIREGUARD_PLATFORM_H_

#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>

// Peers are allocated statically inside the device structure to avoid malloc
// Increased from 1 to support multiple Tailscale peers (10 peers for larger networks)
#define WIREGUARD_MAX_PEERS 40
#define WIREGUARD_MAX_SRC_IPS 2

// Per device limit on accepting (valid) initiation requests - per peer
#define MAX_INITIATIONS_PER_SECOND	(2)

//
// Your platform integration needs to provide implementations of these functions
//

// The number of milliseconds since system boot - for LwIP systems this could be sys_now()
uint32_t wireguard_sys_now();

// Fill the supplied buffer with random data - random data is used for generating new session keys periodically
void wireguard_random_bytes(void *bytes, size_t size);

// Get the current time in tai64n format - 8 byte seconds, 4 byte nano sub-second - see https://cr.yp.to/libtai/tai64.html for details
// Output buffer passed is 12 bytes
// The Wireguard implementation doesn't strictly need this to be a time, but instead an increasing value
// The remote end of the Wireguard tunnel will use this value in handshake replay detection
void wireguard_tai64n_now(uint8_t *output);

// Is the system under load - i.e. should we generate cookie reply message in response to initiation messages
bool wireguard_is_under_load();


#endif /* _WIREGUARD_PLATFORM_H_ */
