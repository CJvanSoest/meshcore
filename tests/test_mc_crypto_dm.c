// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
// SPDX-FileContributor: Ilias el Matani <hello@ilias.codes>
//
// Host test for the DM (TXT_MSG) crypto extracted into mc_crypto: a full
// ed25519-ECDH direct-message round-trip (A encrypts to B, B decrypts), plus
// wrong-recipient and tampered-ciphertext rejection. Links the same vendored
// ed25519.c that runs on the badge, with an esp_random stub so it builds host
// side. This is the most security-critical RX path, previously untested.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ed25519.h"
#include "mc_crypto.h"

// Deterministic stand-in for the ESP RNG ed25519.c feeds to mbedtls ECP
// blinding. ECDH output is independent of it; we only need non-degenerate bytes.
void esp_fill_random(void *buf, size_t len) {
    static uint32_t s = 0x2545f491u;
    uint8_t        *p = buf;
    for (size_t i = 0; i < len; i++) {
        s    = s * 1103515245u + 12345u;
        p[i] = (uint8_t)(s >> 17);
    }
}

static int failures = 0;
#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (!(cond)) { printf("FAIL: %s\n", (msg)); failures++; }               \
    } while (0)

static void keypair(uint8_t pub[32], uint8_t prv[64], uint8_t seedbyte) {
    uint8_t seed[32];
    memset(seed, seedbyte, sizeof seed);
    ed25519_create_keypair(pub, prv, seed);
}

// Frame a DM plaintext: timestamp(4) | flags(1) | text, zero-padded to 16.
static size_t frame(uint8_t *plain, const char *text) {
    uint32_t ts = 0x01020304u;
    memcpy(plain, &ts, 4);
    plain[4]    = 0;
    size_t tl   = strlen(text);
    memcpy(&plain[5], text, tl);
    size_t plen   = 5 + tl;
    size_t padded = ((plen + 15) / 16) * 16;
    for (size_t i = plen; i < padded; i++) plain[i] = 0;
    return padded;
}

// Build the on-wire TXT_MSG payload: dst[1] | src[1] | mac[2] | cipher[...].
static uint8_t build_payload(uint8_t *payload, uint8_t dst0, uint8_t src0,
                             const uint8_t mac[32], const uint8_t *cipher, size_t padded) {
    payload[0] = dst0;
    payload[1] = src0;
    payload[2] = mac[0];
    payload[3] = mac[1];
    memcpy(&payload[4], cipher, padded);
    return (uint8_t)(4 + padded);
}

int main(void) {
    uint8_t a_pub[32], a_prv[64], b_pub[32], b_prv[64], c_pub[32], c_prv[64];
    keypair(a_pub, a_prv, 0x11);
    keypair(b_pub, b_prv, 0x22);
    keypair(c_pub, c_prv, 0x33);

    const char *text = "hello bob, this is alice";
    uint8_t     plain[256] = {0};
    size_t      padded     = frame(plain, text);

    uint8_t cipher[256] = {0}, mac[32];
    mc_crypto_dm_encrypt(b_pub, a_prv, plain, padded, cipher, mac);  // A -> B

    uint8_t payload[256];
    uint8_t payload_len = build_payload(payload, b_pub[0], a_pub[0], mac, cipher, padded);

    // B decrypts (sender is A)
    uint8_t out[256] = {0};
    int     tlen     = 0;
    uint8_t secret[32];
    bool    ok = mc_crypto_dm_decrypt(payload, payload_len, a_pub, b_prv, out, &tlen, secret);
    CHECK(ok, "B decrypts A's DM");
    CHECK(ok && tlen >= (int)strlen(text) && memcmp(out + 5, text, strlen(text)) == 0,
          "recovered DM text matches the original");

    // Wrong recipient C: no candidate shared secret reproduces the MAC.
    uint8_t out2[256] = {0};
    int     tlen2     = 0;
    uint8_t secret2[32];
    CHECK(!mc_crypto_dm_decrypt(payload, payload_len, a_pub, c_prv, out2, &tlen2, secret2),
          "wrong recipient key fails the MAC");

    // Tampered ciphertext fails even for the right recipient.
    payload[6] ^= 0x40;
    uint8_t out3[256] = {0};
    int     tlen3     = 0;
    uint8_t secret3[32];
    CHECK(!mc_crypto_dm_decrypt(payload, payload_len, a_pub, b_prv, out3, &tlen3, secret3),
          "tampered ciphertext fails the MAC");

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_mc_crypto_dm: all checks passed\n");
    return 0;
}
