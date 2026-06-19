// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
// SPDX-FileContributor: Ilias el Matani <hello@ilias.codes>
//
// Host tests for the security-critical MeshCore symmetric crypto extracted into
// mc_crypto: channel (GRP_TXT) encrypt/decrypt, the channel-match property (a
// wrong key fails the MAC), ciphertext-tamper rejection, and the ACK-binding
// CRC. Links the same mc_crypto.c that ships in the firmware, so a regression
// here turns CI red before any IDF build.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mbedtls/sha256.h"

#include "mc_crypto.h"

static int failures = 0;
#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (!(cond)) { printf("FAIL: %s\n", (msg)); failures++; }               \
    } while (0)

// Frame a GRP_TXT plaintext: timestamp(4) | text_type(1) | text, zero-padded to
// a 16-byte multiple. Returns the padded length.
static size_t frame_plain(uint8_t *plain, const char *text) {
    uint32_t ts = 0x11223344u;
    memcpy(plain, &ts, 4);
    plain[4] = 0;
    size_t tlen = strlen(text);
    memcpy(&plain[5], text, tlen);
    size_t plen   = 5 + tlen;
    size_t padded = ((plen + 15) / 16) * 16;
    for (size_t i = plen; i < padded; i++) plain[i] = 0;
    return padded;
}

static void fill_key(uint8_t *key, size_t n, uint8_t base) {
    for (size_t i = 0; i < n; i++) key[i] = (uint8_t)(base + i);
}

static void make_grp(meshcore_grp_txt_t *grp, const uint8_t *key, const char *text) {
    uint8_t plain[256] = {0};
    size_t  padded     = frame_plain(plain, text);
    uint8_t cipher[256] = {0}, mac[32];
    mc_crypto_grp_encrypt(key, plain, padded, cipher, mac);
    memset(grp, 0, sizeof(*grp));
    grp->data_length = (uint8_t)padded;
    memcpy(grp->data, cipher, padded);
    memcpy(grp->mac, mac, MESHCORE_CIPHER_MAC_SIZE);
}

static void test_grp_roundtrip(void) {
    uint8_t key[MESHCORE_CIPHER_KEY_SIZE];
    fill_key(key, sizeof key, 0xA0);
    const char *text = "hello channel";

    meshcore_grp_txt_t grp;
    make_grp(&grp, key, text);
    bool ok = mc_crypto_grp_decrypt(&grp, key);
    CHECK(ok, "round-trip decrypt should succeed");
    CHECK(ok && strcmp(grp.decrypted.text, text) == 0, "round-trip text matches");
}

static void test_grp_wrong_key(void) {
    uint8_t key[MESHCORE_CIPHER_KEY_SIZE], wrong[MESHCORE_CIPHER_KEY_SIZE];
    fill_key(key, sizeof key, 0xA0);
    fill_key(wrong, sizeof wrong, 0x10);

    meshcore_grp_txt_t grp;
    make_grp(&grp, key, "secret");
    CHECK(!mc_crypto_grp_decrypt(&grp, wrong), "wrong channel key must fail the MAC");
}

static void test_grp_tampered(void) {
    uint8_t key[MESHCORE_CIPHER_KEY_SIZE];
    fill_key(key, sizeof key, 0xA0);

    meshcore_grp_txt_t grp;
    make_grp(&grp, key, "integrity");
    grp.data[0] ^= 0x01;  // flip a ciphertext bit
    CHECK(!mc_crypto_grp_decrypt(&grp, key), "tampered ciphertext must fail the MAC");
}

static void test_ack_crc(void) {
    uint8_t     head5[5] = {0x44, 0x33, 0x22, 0x11, 0x00};
    const char *text     = "ack me";
    uint8_t     pub[MESHCORE_PUB_KEY_SIZE];
    fill_key(pub, sizeof pub, 0x00);

    uint8_t crc[4];
    mc_crypto_ack_crc(head5, text, strlen(text), pub, crc);

    // Independent recomputation per the documented spec.
    uint8_t                sha[32];
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    mbedtls_sha256_update(&c, head5, 5);
    mbedtls_sha256_update(&c, (const uint8_t *)text, strlen(text));
    mbedtls_sha256_update(&c, pub, MESHCORE_PUB_KEY_SIZE);
    mbedtls_sha256_finish(&c, sha);
    mbedtls_sha256_free(&c);
    CHECK(memcmp(crc, sha, 4) == 0, "ack crc equals SHA256(head5||text||pub)[0:4]");

    // The binding must depend on the pubkey.
    uint8_t pub2[MESHCORE_PUB_KEY_SIZE];
    memcpy(pub2, pub, sizeof pub2);
    pub2[0] ^= 0xFF;
    uint8_t crc2[4];
    mc_crypto_ack_crc(head5, text, strlen(text), pub2, crc2);
    CHECK(memcmp(crc, crc2, 4) != 0, "ack crc is bound to the pubkey");
}

int main(void) {
    test_grp_roundtrip();
    test_grp_wrong_key();
    test_grp_tampered();
    test_ack_crc();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_mc_crypto: all checks passed\n");
    return 0;
}
