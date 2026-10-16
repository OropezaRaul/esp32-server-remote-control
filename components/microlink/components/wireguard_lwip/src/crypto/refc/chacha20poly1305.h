































#ifndef _CHACHA20POLY1305_H_
#define _CHACHA20POLY1305_H_

#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>

// Aead(key, counter, plain text, auth text) ChaCha20Poly1305 AEAD, as specified in RFC7539 [17], with its nonce being composed of 32 bits of zeros followed by the 64-bit little-endian value of counter.
// AEAD_CHACHA20_POLY1305 as described in https://tools.ietf.org/html/rfc7539
void chacha20poly1305_encrypt(uint8_t *dst, const uint8_t *src, size_t src_len, const uint8_t *ad, size_t ad_len, uint64_t nonce, const uint8_t *key);
bool chacha20poly1305_decrypt(uint8_t *dst, const uint8_t *src, size_t src_len, const uint8_t *ad, size_t ad_len, uint64_t nonce, const uint8_t *key);

// Xaead(key, nonce, plain text, auth text) XChaCha20Poly1305 AEAD, with a 24-byte random nonce, instantiated using HChaCha20 [6] and ChaCha20Poly1305.
// AEAD_XChaCha20_Poly1305 as described in https://tools.ietf.org/id/draft-arciszewski-xchacha-02.html
void xchacha20poly1305_encrypt(uint8_t *dst, const uint8_t *src, size_t src_len, const uint8_t *ad, size_t ad_len, const uint8_t *nonce, const uint8_t *key);
bool xchacha20poly1305_decrypt(uint8_t *dst, const uint8_t *src, size_t src_len, const uint8_t *ad, size_t ad_len, const uint8_t *nonce, const uint8_t *key);

#endif 
