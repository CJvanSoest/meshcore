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
#include "mbedtls/aes.h"
#include "mbedtls/md.h"
#include "mc_crypto.h"

// Deterministic stand-in for the ESP RNG ed25519.c feeds to mbedtls ECP
// blinding. ECDH output is independent of it; we only need non-degenerate bytes.
void esp_fill_random(void* buf, size_t len) {
    static uint32_t s = 0x2545f491u;
    uint8_t*        p = buf;
    for (size_t i = 0; i < len; i++) {
        s    = s * 1103515245u + 12345u;
        p[i] = (uint8_t)(s >> 17);
    }
}

static int failures = 0;
#define CHECK(cond, msg)                 \
    do {                                 \
        if (!(cond)) {                   \
            printf("FAIL: %s\n", (msg)); \
            failures++;                  \
        }                                \
    } while (0)

static void keypair(uint8_t pub[32], uint8_t prv[64], uint8_t seedbyte) {
    uint8_t seed[32];
    memset(seed, seedbyte, sizeof seed);
    ed25519_create_keypair(pub, prv, seed);
}

// Frame a DM plaintext: timestamp(4) | flags(1) | text, zero-padded to 16.
static size_t frame(uint8_t* plain, const char* text) {
    uint32_t ts = 0x01020304u;
    memcpy(plain, &ts, 4);
    plain[4]  = 0;
    size_t tl = strlen(text);
    memcpy(&plain[5], text, tl);
    size_t plen   = 5 + tl;
    size_t padded = ((plen + 15) / 16) * 16;
    for (size_t i = plen; i < padded; i++) plain[i] = 0;
    return padded;
}

// Build the on-wire TXT_MSG payload: dst[1] | src[1] | mac[2] | cipher[...].
static uint8_t build_payload(uint8_t* payload, uint8_t dst0, uint8_t src0, const uint8_t mac[32], const uint8_t* cipher,
                             size_t padded) {
    payload[0] = dst0;
    payload[1] = src0;
    payload[2] = mac[0];
    payload[3] = mac[1];
    memcpy(&payload[4], cipher, padded);
    return (uint8_t)(4 + padded);
}

// Encrypt a DM the way a peer keyed one of the four interop variants:
// converted vs raw ed25519->x25519 secret, MAC'd with a 16- or 32-byte HMAC
// key. AES always uses the first 16 bytes of the same secret. mc_crypto_dm.c's
// own encrypt only ever produces the converted/32 case, so this is the only way
// to exercise the other three accept branches.
// Derive the secret exactly as the receiver will (sender_pub, recipient_prv).
// The converted variant is symmetric so either order works, but the raw variant
// uses the Edwards bytes directly as the u-coordinate and is NOT symmetric, so
// to land on the receiver's secret_raw we must key from the receiver's side.
static void encrypt_variant(const uint8_t sender_pub[32], const uint8_t recipient_prv[64], const uint8_t* plain,
                            size_t padded, bool raw, int keylen, uint8_t* out_cipher, uint8_t out_mac[32]) {
    uint8_t secret[32];
    if (raw)
        ed25519_key_exchange_raw(secret, sender_pub, recipient_prv);
    else
        ed25519_key_exchange(secret, sender_pub, recipient_prv);

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, secret, 128);
    for (size_t i = 0; i < padded / 16; i++)
        mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, &plain[i * 16], &out_cipher[i * 16]);
    mbedtls_aes_free(&aes);

    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), secret, (size_t)keylen, out_cipher, padded, out_mac);
}

static void test_variant(const char* name, const uint8_t b_pub[32], const uint8_t a_pub[32], const uint8_t b_prv[64],
                         bool raw, int keylen) {
    const char* text        = "variant probe message";
    uint8_t     plain[256]  = {0};
    size_t      padded      = frame(plain, text);
    uint8_t     cipher[256] = {0}, mac[32];
    encrypt_variant(a_pub, b_prv, plain, padded, raw, keylen, cipher, mac);

    uint8_t payload[256];
    uint8_t plen     = build_payload(payload, b_pub[0], a_pub[0], mac, cipher, padded);
    uint8_t out[256] = {0};
    int     tlen     = 0;
    uint8_t secret[32];
    bool    ok = mc_crypto_dm_decrypt(payload, plen, a_pub, b_prv, out, &tlen, secret);
    CHECK(ok && memcmp(out + 5, text, strlen(text)) == 0, name);
}

int main(void) {
    uint8_t a_pub[32], a_prv[64], b_pub[32], b_prv[64], c_pub[32], c_prv[64];
    keypair(a_pub, a_prv, 0x11);
    keypair(b_pub, b_prv, 0x22);
    keypair(c_pub, c_prv, 0x33);

    const char* text       = "hello bob, this is alice";
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
    payload[6]        ^= 0x40;
    uint8_t out3[256]  = {0};
    int     tlen3      = 0;
    uint8_t secret3[32];
    CHECK(!mc_crypto_dm_decrypt(payload, payload_len, a_pub, b_prv, out3, &tlen3, secret3),
          "tampered ciphertext fails the MAC");

    // The other three accept variants a peer may have keyed (the converted/32
    // case above is the one mc_crypto_dm_encrypt produces).
    test_variant("decrypts the raw-secret / 32-byte HMAC variant", b_pub, a_pub, b_prv, true, 32);
    test_variant("decrypts the converted-secret / 16-byte HMAC variant", b_pub, a_pub, b_prv, false, 16);
    test_variant("decrypts the raw-secret / 16-byte HMAC variant", b_pub, a_pub, b_prv, true, 16);

    // A payload too short to hold dst|src|mac + one AES block is rejected.
    uint8_t shortp[8] = {0};
    int     stlen     = 0;
    uint8_t ssecret[32];
    CHECK(!mc_crypto_dm_decrypt(shortp, (uint8_t)sizeof shortp, a_pub, b_prv, out, &stlen, ssecret),
          "a too-short DM payload is rejected");

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_mc_crypto_dm: all checks passed\n");
    return 0;
}
