// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// X25519 extras layered on the vendored orlp/ed25519 field arithmetic (fe.h):
//   - ed25519_pub_to_x25519:  Edwards y -> Montgomery u  (u = (1+y)/(1-y))
//   - ed25519_key_exchange_raw: the Montgomery ladder with the public key bytes
//     taken directly as the u-coordinate (no Edwards->Montgomery step).
//
// The ladder is orlp's ed25519_key_exchange verbatim minus the conversion block,
// so the two share the same well-tested arithmetic. Both are exercised by
// identity.c's boot self-test and tests/test_mc_crypto_dm.c.

#include "ed25519.h"
#include "ed25519/fe.h"

void ed25519_pub_to_x25519(uint8_t* curve25519_pub, const uint8_t* ed25519_pub) {
    fe y, tmp0, tmp1;
    fe_frombytes(y, ed25519_pub);
    fe_1(tmp1);
    fe_add(tmp0, y, tmp1);  // 1 + y
    fe_sub(tmp1, tmp1, y);  // 1 - y
    fe_invert(tmp1, tmp1);  // 1 / (1 - y)
    fe_mul(y, tmp0, tmp1);  // (1 + y) / (1 - y)
    fe_tobytes(curve25519_pub, y);
}

void ed25519_key_exchange_raw(uint8_t* shared_secret, const uint8_t* public_key, const uint8_t* private_key) {
    unsigned char e[32];
    unsigned int  i;

    fe x1, x2, z2, x3, z3, tmp0, tmp1;

    int          pos;
    unsigned int swap;
    unsigned int b;

    for (i = 0; i < 32; ++i) e[i] = private_key[i];
    e[0]  &= 248;
    e[31] &= 63;
    e[31] |= 64;

    // Raw: use the public key bytes directly as the Montgomery u-coordinate.
    fe_frombytes(x1, public_key);

    fe_1(x2);
    fe_0(z2);
    fe_copy(x3, x1);
    fe_1(z3);

    swap = 0;
    for (pos = 254; pos >= 0; --pos) {
        b     = e[pos / 8] >> (pos & 7);
        b    &= 1;
        swap ^= b;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = b;

        fe_sub(tmp0, x3, z3);
        fe_sub(tmp1, x2, z2);
        fe_add(x2, x2, z2);
        fe_add(z2, x3, z3);
        fe_mul(z3, tmp0, x2);
        fe_mul(z2, z2, tmp1);
        fe_sq(tmp0, tmp1);
        fe_sq(tmp1, x2);
        fe_add(x3, z3, z2);
        fe_sub(z2, z3, z2);
        fe_mul(x2, tmp1, tmp0);
        fe_sub(tmp1, tmp1, tmp0);
        fe_sq(z2, z2);
        fe_mul121666(z3, tmp1);
        fe_sq(x3, x3);
        fe_add(tmp0, tmp0, z3);
        fe_mul(z3, x1, z2);
        fe_mul(z2, tmp1, tmp0);
    }

    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);

    fe_invert(z2, z2);
    fe_mul(x2, x2, z2);
    fe_tobytes(shared_secret, x2);
}
