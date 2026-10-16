






#ifndef _NACL_BOX_H_
#define _NACL_BOX_H_

#include <stdint.h>
#include <stddef.h>

#define NACL_BOX_NONCEBYTES 24
#define NACL_BOX_MACBYTES 16
#define NACL_BOX_PUBLICKEYBYTES 32
#define NACL_BOX_SECRETKEYBYTES 32














int nacl_box(uint8_t *ciphertext,
             const uint8_t *plaintext, size_t plaintext_len,
             const uint8_t *nonce,
             const uint8_t *recipient_pk,
             const uint8_t *sender_sk);









int nacl_box_beforenm(uint8_t *shared_key,
                      const uint8_t *recipient_pk,
                      const uint8_t *sender_sk);











int nacl_box_afternm(uint8_t *ciphertext,
                     const uint8_t *plaintext, size_t plaintext_len,
                     const uint8_t *nonce,
                     const uint8_t *shared_key);














int nacl_box_open(uint8_t *plaintext,
                  const uint8_t *ciphertext, size_t ciphertext_len,
                  const uint8_t *nonce,
                  const uint8_t *sender_pk,
                  const uint8_t *recipient_sk);











int nacl_box_open_afternm(uint8_t *plaintext,
                          const uint8_t *ciphertext, size_t ciphertext_len,
                          const uint8_t *nonce,
                          const uint8_t *shared_key);

#endif 
