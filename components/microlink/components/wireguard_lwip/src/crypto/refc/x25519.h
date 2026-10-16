// Taken from https://sourceforge.net/p/strobe (MIT Licence)









#ifndef __X25519_H__
#define __X25519_H__

#define X25519_BYTES (256/8)


extern const unsigned char X25519_BASE_POINT[X25519_BYTES];


#define EC_PUBLIC_BYTES 32


#define EC_PRIVATE_BYTES 32





#define EC_CHALLENGE_BYTES 32





#define EC_UNIFORM_BYTES 32























int x25519 (
    unsigned char out[EC_PUBLIC_BYTES],
    const unsigned char scalar[EC_PRIVATE_BYTES],
    const unsigned char base[EC_PUBLIC_BYTES],
    int clamp
);














static inline int x25519_base (
    unsigned char out[EC_PUBLIC_BYTES],
    const unsigned char scalar[EC_PRIVATE_BYTES],
    int clamp
) {
    return x25519(out,scalar,X25519_BASE_POINT,clamp);
}








static inline void x25519_base_uniform (
    unsigned char out[EC_PUBLIC_BYTES],
    const unsigned char scalar[EC_UNIFORM_BYTES]
) {
    (void)x25519_base(out,scalar,0);
}








void x25519_sign_p2 (
    unsigned char response[EC_PRIVATE_BYTES],
    const unsigned char challenge[EC_CHALLENGE_BYTES],
    const unsigned char eph_secret[EC_UNIFORM_BYTES],
    const unsigned char secret[EC_PRIVATE_BYTES]
);








int x25519_verify_p2 (
    const unsigned char response[X25519_BYTES],
    const unsigned char challenge[X25519_BYTES],
    const unsigned char eph[X25519_BYTES],
    const unsigned char pub[X25519_BYTES]
);

#endif 
