































// RFC7539 implementation of ChaCha20 with modified nonce size for WireGuard
// https://tools.ietf.org/html/rfc7539
// Adapted from https://cr.yp.to/streamciphers/timings/estreambench/submissions/salsa20/chacha8/ref/chacha.c by D. J. Bernstein (Public Domain)
// HChaCha20 is described here: https://tools.ietf.org/id/draft-arciszewski-xchacha-02.html
#ifndef _CHACHA20_H_
#define _CHACHA20_H_

#include <stdint.h>

#define CHACHA20_BLOCK_SIZE		(64)
#define CHACHA20_KEY_SIZE		(32)

struct chacha20_ctx {
	uint32_t state[16];
};

void chacha20_init(struct chacha20_ctx *ctx, const uint8_t *key, const uint64_t nonce);
void chacha20(struct chacha20_ctx *ctx, uint8_t *out, const uint8_t *in, uint32_t len);
void hchacha20(uint8_t *out, const uint8_t *nonce, const uint8_t *key);

#endif 
