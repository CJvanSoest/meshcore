// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// Host-side test for the vendored AES-128 (tiny-AES) + the mc_aes wrapper that
// replaced mbedtls/aes.h for the ESP-IDF 6 migration. Checks the wrapper output
// against the NIST SP800-38A worked example vectors (ECB F.1.1, CBC F.2.1) and
// a decrypt round-trip, so a wrong key schedule or a CBC IV/chaining slip makes
// CI red before it can reach the crypto call sites.
//
// Build (see tests/Makefile):
//     gcc test_mc_aes.c ../components/vendor/mc_aes.c ../components/vendor/aes/aes.c -o test_mc_aes
//
// Exit 0 on pass, 1 on any mismatch.

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "mc_aes.h"

static int failures = 0;

#define CHECK(cond, name)                            \
    do {                                             \
        if (cond) {                                  \
            printf("  %-28s PASS\n", name);          \
        } else {                                     \
            fprintf(stderr, "  %-28s FAIL\n", name); \
            failures++;                              \
        }                                            \
    } while (0)

int main(void) {
    // NIST SP800-38A F.1.1 / F.2.1 shared key.
    const uint8_t key[16] = {0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
                             0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};

    printf("mc_aes host-side NIST SP800-38A self-test\n\n");

    // ── ECB (F.1.1), single block ────────────────────────────────────────────
    const uint8_t ecb_pt[16] = {0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
                                0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a};
    const uint8_t ecb_ct[16] = {0x3a, 0xd7, 0x7b, 0xb4, 0x0d, 0x7a, 0x36, 0x60,
                                0xa8, 0x9e, 0xca, 0xf3, 0x24, 0x66, 0xef, 0x97};
    mc_aes_t      a;
    mc_aes128_init(&a, key);
    uint8_t out[32], back[32];
    mc_aes128_ecb_encrypt(&a, ecb_pt, out);
    CHECK(memcmp(out, ecb_ct, 16) == 0, "ECB encrypt vector");
    mc_aes128_ecb_decrypt(&a, ecb_ct, back);
    CHECK(memcmp(back, ecb_pt, 16) == 0, "ECB decrypt vector");

    // ── CBC (F.2.1), two blocks ──────────────────────────────────────────────
    const uint8_t iv[16]     = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    const uint8_t cbc_pt[32] = {0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e,
                                0x11, 0x73, 0x93, 0x17, 0x2a, 0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03,
                                0xac, 0x9c, 0x9e, 0xb7, 0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51};
    const uint8_t cbc_ct[32] = {0x76, 0x49, 0xab, 0xac, 0x81, 0x19, 0xb2, 0x46, 0xce, 0xe9, 0x8e,
                                0x9b, 0x12, 0xe9, 0x19, 0x7d, 0x50, 0x86, 0xcb, 0x9b, 0x50, 0x72,
                                0x19, 0xee, 0x95, 0xdb, 0x11, 0x3a, 0x91, 0x76, 0x78, 0xb2};
    mc_aes128_init(&a, key);  // fresh key schedule
    mc_aes128_cbc_encrypt(&a, iv, 32, cbc_pt, out);
    CHECK(memcmp(out, cbc_ct, 32) == 0, "CBC encrypt vector");
    mc_aes128_init(&a, key);
    mc_aes128_cbc_decrypt(&a, iv, 32, cbc_ct, back);
    CHECK(memcmp(back, cbc_pt, 32) == 0, "CBC decrypt vector");

    if (failures == 0)
        printf("\ntest_mc_aes: OK\n");
    else
        printf("\ntest_mc_aes: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
