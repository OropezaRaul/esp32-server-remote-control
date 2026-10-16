















#include "microlink_internal.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/chacha20.h"
#include "mbedtls/chachapoly.h"
#include <string.h>

static const char *TAG = "ml_noise";


#include "x25519.h"


#include "blake2s.h"


static const uint8_t TAILSCALE_SERVER_PUBLIC_KEY[32] = {
    0x7d, 0x27, 0x92, 0xf9, 0xc9, 0x8d, 0x75, 0x3d,
    0x20, 0x42, 0x47, 0x15, 0x36, 0x80, 0x19, 0x49,
    0x10, 0x4c, 0x24, 0x7f, 0x95, 0xea, 0xc7, 0x70,
    0xf8, 0xfb, 0x32, 0x15, 0x95, 0xe2, 0x17, 0x3b
};


static const char NOISE_PROTOCOL_NAME[] = "Noise_IK_25519_ChaChaPoly_BLAKE2s";










static void noise_hash(uint8_t out[32], const uint8_t *data, size_t len) {
    blake2s(out, 32, NULL, 0, data, len);
}




static void hmac_blake2s(uint8_t out[32],
                          const uint8_t *key, size_t key_len,
                          const uint8_t *data, size_t data_len) {
    uint8_t ipad[BLAKE2S_BLOCK_SIZE];
    uint8_t opad[BLAKE2S_BLOCK_SIZE];
    uint8_t key_block[BLAKE2S_BLOCK_SIZE];
    uint8_t inner_hash[32];

    
    if (key_len > BLAKE2S_BLOCK_SIZE) {
        noise_hash(key_block, key, key_len);
        key_len = 32;
    } else {
        memcpy(key_block, key, key_len);
    }
    
    if (key_len < BLAKE2S_BLOCK_SIZE) {
        memset(key_block + key_len, 0, BLAKE2S_BLOCK_SIZE - key_len);
    }

    
    for (int i = 0; i < BLAKE2S_BLOCK_SIZE; i++) {
        ipad[i] = key_block[i] ^ 0x36;
        opad[i] = key_block[i] ^ 0x5c;
    }

    blake2s_ctx ctx;
    blake2s_init(&ctx, 32, NULL, 0);
    blake2s_update(&ctx, ipad, BLAKE2S_BLOCK_SIZE);
    blake2s_update(&ctx, data, data_len);
    blake2s_final(&ctx, inner_hash);

    
    blake2s_init(&ctx, 32, NULL, 0);
    blake2s_update(&ctx, opad, BLAKE2S_BLOCK_SIZE);
    blake2s_update(&ctx, inner_hash, 32);
    blake2s_final(&ctx, out);

    memset(key_block, 0, sizeof(key_block));
    memset(inner_hash, 0, sizeof(inner_hash));
}






static void noise_hkdf(const uint8_t *ck, const uint8_t *input, size_t input_len,
                         uint8_t *out1, uint8_t *out2, uint8_t *out3) {
    
    uint8_t temp_key[32];
    hmac_blake2s(temp_key, ck, 32, input, input_len);

    
    uint8_t one = 0x01;
    hmac_blake2s(out1, temp_key, 32, &one, 1);

    if (out2) {
        
        uint8_t buf[33];
        memcpy(buf, out1, 32);
        buf[32] = 0x02;
        hmac_blake2s(out2, temp_key, 32, buf, 33);

        if (out3) {
            
            memcpy(buf, out2, 32);
            buf[32] = 0x03;
            hmac_blake2s(out3, temp_key, 32, buf, 33);
        }
    }

    memset(temp_key, 0, sizeof(temp_key));
}


static void noise_mix_hash(uint8_t h[32], const uint8_t *data, size_t data_len) {
    blake2s_ctx ctx;
    blake2s_init(&ctx, 32, NULL, 0);
    blake2s_update(&ctx, h, 32);
    blake2s_update(&ctx, data, data_len);
    blake2s_final(&ctx, h);
}


static void noise_mix_key(uint8_t ck[32], uint8_t k[32], const uint8_t *dh_output) {
    noise_hkdf(ck, dh_output, 32, ck, k, NULL);
}





static int chacha20poly1305_encrypt(const uint8_t *key, uint64_t nonce,
                                      const uint8_t *ad, size_t ad_len,
                                      const uint8_t *plaintext, size_t pt_len,
                                      uint8_t *ciphertext) {
    


    uint8_t nonce_bytes[12];
    memset(nonce_bytes, 0, 4);
    nonce_bytes[4]  = (nonce >> 56) & 0xFF;
    nonce_bytes[5]  = (nonce >> 48) & 0xFF;
    nonce_bytes[6]  = (nonce >> 40) & 0xFF;
    nonce_bytes[7]  = (nonce >> 32) & 0xFF;
    nonce_bytes[8]  = (nonce >> 24) & 0xFF;
    nonce_bytes[9]  = (nonce >> 16) & 0xFF;
    nonce_bytes[10] = (nonce >> 8) & 0xFF;
    nonce_bytes[11] = nonce & 0xFF;

    mbedtls_chachapoly_context ctx;
    mbedtls_chachapoly_init(&ctx);
    mbedtls_chachapoly_setkey(&ctx, key);

    
    int ret = mbedtls_chachapoly_encrypt_and_tag(&ctx,
                pt_len, nonce_bytes, ad, ad_len,
                plaintext, ciphertext, ciphertext + pt_len);

    mbedtls_chachapoly_free(&ctx);
    return ret;
}

static int chacha20poly1305_decrypt(const uint8_t *key, uint64_t nonce,
                                      const uint8_t *ad, size_t ad_len,
                                      const uint8_t *ciphertext, size_t ct_len,
                                      uint8_t *plaintext) {
    if (ct_len < 16) return -1;

    
    uint8_t nonce_bytes[12];
    memset(nonce_bytes, 0, 4);
    nonce_bytes[4]  = (nonce >> 56) & 0xFF;
    nonce_bytes[5]  = (nonce >> 48) & 0xFF;
    nonce_bytes[6]  = (nonce >> 40) & 0xFF;
    nonce_bytes[7]  = (nonce >> 32) & 0xFF;
    nonce_bytes[8]  = (nonce >> 24) & 0xFF;
    nonce_bytes[9]  = (nonce >> 16) & 0xFF;
    nonce_bytes[10] = (nonce >> 8) & 0xFF;
    nonce_bytes[11] = nonce & 0xFF;

    size_t payload_len = ct_len - 16;
    const uint8_t *tag = ciphertext + payload_len;

    mbedtls_chachapoly_context ctx;
    mbedtls_chachapoly_init(&ctx);
    mbedtls_chachapoly_setkey(&ctx, key);

    int ret = mbedtls_chachapoly_auth_decrypt(&ctx,
                payload_len, nonce_bytes, ad, ad_len,
                tag, ciphertext, plaintext);

    mbedtls_chachapoly_free(&ctx);
    return ret;
}





void ml_noise_init(ml_noise_state_t *state,
                    const uint8_t *local_private, const uint8_t *local_public,
                    const uint8_t *remote_public) {
    memset(state, 0, sizeof(*state));

    
    memcpy(state->local_static_private, local_private, 32);
    memcpy(state->local_static_public, local_public, 32);

    if (remote_public) {
        memcpy(state->remote_static_public, remote_public, 32);
    } else {
        memcpy(state->remote_static_public, TAILSCALE_SERVER_PUBLIC_KEY, 32);
    }

    
    noise_hash(state->h, (const uint8_t *)NOISE_PROTOCOL_NAME,
               strlen(NOISE_PROTOCOL_NAME));

    
    memcpy(state->ck, state->h, 32);

    
    char prologue[64];
    int prologue_len = snprintf(prologue, sizeof(prologue),
                                "Tailscale Control Protocol v%d",
                                ML_CTRL_PROTOCOL_VER);
    noise_mix_hash(state->h, (const uint8_t *)prologue, prologue_len);

    
    noise_mix_hash(state->h, state->remote_static_public, 32);

    
    esp_fill_random(state->local_ephemeral_private, 32);
    state->local_ephemeral_private[0] &= 248;
    state->local_ephemeral_private[31] &= 127;
    state->local_ephemeral_private[31] |= 64;
    x25519_base(state->local_ephemeral_public, state->local_ephemeral_private, 1);

    state->handshake_complete = false;

    ESP_LOGI(TAG, "Noise IK initialized (protocol v%d)", ML_CTRL_PROTOCOL_VER);
}








esp_err_t ml_noise_write_msg1(ml_noise_state_t *state, uint8_t *out, size_t *out_len) {
    size_t pos = 0;

    
    out[pos++] = (ML_CTRL_PROTOCOL_VER >> 8) & 0xFF;  
    out[pos++] = ML_CTRL_PROTOCOL_VER & 0xFF;          
    out[pos++] = 0x01;  
    out[pos++] = 0x00;
    out[pos++] = 0x60;  

    
    memcpy(out + pos, state->local_ephemeral_public, 32);
    noise_mix_hash(state->h, state->local_ephemeral_public, 32);
    pos += 32;

    
    uint8_t dh_output[32];
    x25519(dh_output, state->local_ephemeral_private, state->remote_static_public, 1);

    uint8_t k[32];
    noise_mix_key(state->ck, k, dh_output);
    memset(dh_output, 0, 32);

    
    chacha20poly1305_encrypt(k, 0, state->h, 32,
                              state->local_static_public, 32,
                              out + pos);
    noise_mix_hash(state->h, out + pos, 48);
    pos += 48;

    
    x25519(dh_output, state->local_static_private, state->remote_static_public, 1);
    noise_mix_key(state->ck, k, dh_output);
    memset(dh_output, 0, 32);

    
    chacha20poly1305_encrypt(k, 0, state->h, 32,
                              NULL, 0, out + pos);
    noise_mix_hash(state->h, out + pos, 16);
    pos += 16;

    memset(k, 0, 32);
    *out_len = pos;

    ESP_LOGI(TAG, "Noise msg1 built: %d bytes", (int)pos);
    return ESP_OK;
}









esp_err_t ml_noise_read_msg2(ml_noise_state_t *state, const uint8_t *msg, size_t len) {
    if (len < 32 + 16) {
        ESP_LOGE(TAG, "Noise msg2 too short: %d (need >= 48)", (int)len);
        return ESP_FAIL;
    }

    size_t offset = 0;
    uint8_t remote_ephemeral[32];
    uint8_t dh_output[32];
    uint8_t k[32];

    
    memcpy(remote_ephemeral, msg + offset, 32);
    noise_mix_hash(state->h, remote_ephemeral, 32);
    offset += 32;

    
    x25519(dh_output, state->local_ephemeral_private, remote_ephemeral, 1);
    noise_mix_key(state->ck, k, dh_output);

    
    x25519(dh_output, state->local_static_private, remote_ephemeral, 1);
    noise_mix_key(state->ck, k, dh_output);
    memset(dh_output, 0, 32);

    
    size_t ciphertext_len = len - offset;
    if (chacha20poly1305_decrypt(k, 0, state->h, 32,
                                  msg + offset, ciphertext_len,
                                  NULL) != 0) {
        ESP_LOGE(TAG, "Noise msg2 auth tag verification failed");
        memset(k, 0, 32);
        return ESP_ERR_INVALID_MAC;
    }
    noise_mix_hash(state->h, msg + offset, ciphertext_len);

    
    noise_hkdf(state->ck, NULL, 0, state->tx_key, state->rx_key, NULL);
    state->tx_nonce = 0;
    state->rx_nonce = 0;
    state->handshake_complete = true;

    memset(k, 0, 32);
    ESP_LOGI(TAG, "Noise handshake complete - transport keys derived");
    return ESP_OK;
}





esp_err_t ml_noise_encrypt(const uint8_t *key, uint64_t nonce,
                            const uint8_t *ad, size_t ad_len,
                            const uint8_t *plaintext, size_t pt_len,
                            uint8_t *ciphertext) {
    if (chacha20poly1305_encrypt(key, nonce, ad, ad_len,
                                  plaintext, pt_len, ciphertext) != 0) {
        ESP_LOGE(TAG, "Noise encrypt failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t ml_noise_decrypt(const uint8_t *key, uint64_t nonce,
                            const uint8_t *ad, size_t ad_len,
                            const uint8_t *ciphertext, size_t ct_len,
                            uint8_t *plaintext) {
    if (chacha20poly1305_decrypt(key, nonce, ad, ad_len,
                                  ciphertext, ct_len, plaintext) != 0) {
        ESP_LOGE(TAG, "Noise decrypt failed (nonce=%llu)", (unsigned long long)nonce);
        return ESP_ERR_INVALID_MAC;
    }
    return ESP_OK;
}
