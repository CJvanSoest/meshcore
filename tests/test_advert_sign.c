// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
// SPDX-FileContributor: Ilias el Matani <hello@ilias.codes>
//
// Host test for the ADVERT signature LAYOUT -- the byte range that
// meshcore_advert_signable_bytes() feeds to ed25519_sign(). The sign math is
// already gated by test_ed25519 (RFC 8032 vectors); this guards the other
// half of "will upstream accept our advert": that we sign pub_key + timestamp
// + the post-signature tail and skip the 64-byte signature slot. An off-by-one
// here produces a valid-but-rejected signature -- the original direct-advert
// bug. Two independent checks: the offsets (computed straight from the wire
// layout, not via the helper) and a golden signature over a fixed advert.
//
// Links the same advert.c + advert_sign.c + ed25519_mpi.c that ship on the
// badge. Exit 0 on pass, 1 on any mismatch.

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "advert_sign.h"
#include "ed25519_mpi.h"
#include "meshcore/packet.h"
#include "meshcore/payload/advert.h"

static int failures = 0;
#define CHECK(cond, msg)                 \
    do {                                 \
        if (!(cond)) {                   \
            printf("FAIL: %s\n", (msg)); \
            failures++;                  \
        }                                \
    } while (0)

// RFC 8032 TV1 seed -- deterministic keypair, so the golden signature is fixed.
static const uint8_t SEED[32] = {0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60, 0xba, 0x84, 0x4a,
                                 0xf4, 0x92, 0xec, 0x2c, 0xc4, 0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32,
                                 0x69, 0x19, 0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60};

#define FIXED_TS 0x60123456u

// Golden signature over the fixed advert below, recomputed independently if the
// advert wire layout or the ed25519 impl ever changes.
static const uint8_t GOLDEN_SIG[64] = {0x72, 0x26, 0x9c, 0x4a, 0xa2, 0xd6, 0x4d, 0x72, 0x42, 0x4a, 0xc4, 0x72, 0x63,
                                       0x3d, 0xe1, 0xc4, 0xb8, 0xea, 0x7d, 0xb3, 0x78, 0xd6, 0x30, 0x13, 0xac, 0xb1,
                                       0xe4, 0x50, 0x7a, 0x32, 0x47, 0x2b, 0xaa, 0x46, 0x89, 0xf5, 0x0b, 0x27, 0x96,
                                       0xf6, 0x9d, 0x2a, 0x94, 0xc4, 0x11, 0x19, 0xd4, 0x9c, 0x10, 0x29, 0x58, 0x7b,
                                       0xdd, 0x3d, 0x16, 0xf5, 0xba, 0x8e, 0xfd, 0x32, 0x86, 0xb9, 0x59, 0x02};

int main(void) {
    uint8_t pub[32], prv[64];
    ed25519_create_keypair(pub, prv, SEED);

    meshcore_advert_t adv = {0};
    memcpy(adv.pub_key, pub, MESHCORE_PUB_KEY_SIZE);
    adv.timestamp = FIXED_TS;
    adv.role      = 1;
    strncpy(adv.name, "Tanmatsu-TST", MESHCORE_MAX_NAME_SIZE);
    adv.name_valid = true;

    uint8_t payload[MESHCORE_MAX_PAYLOAD_SIZE];
    uint8_t payload_len = 0;
    CHECK(meshcore_advert_serialize(&adv, payload, &payload_len) >= 0, "advert serialize");

    uint8_t to_sign[MESHCORE_MAX_PAYLOAD_SIZE];
    uint8_t to_sign_len = meshcore_advert_signable_bytes(payload, payload_len, to_sign);

    // --- Layout, checked from the wire offsets independently of the helper ---
    const uint8_t head      = MESHCORE_PUB_KEY_SIZE + 4;  // pub_key + ts
    const uint8_t after_sig = head + MESHCORE_SIGNATURE_SIZE;
    const uint8_t tail      = payload_len - after_sig;
    CHECK(to_sign_len == head + tail, "signable length = head + post-signature tail");
    CHECK(memcmp(to_sign, payload, head) == 0, "signable starts with pub_key + timestamp");
    CHECK(memcmp(to_sign + head, payload + after_sig, tail) == 0, "signable continues past the 64-byte signature slot");

    // --- Golden signature over the signed bytes ---
    uint8_t sig[64];
    ed25519_sign(sig, to_sign, to_sign_len, pub, prv);
    CHECK(memcmp(sig, GOLDEN_SIG, 64) == 0, "advert signature matches the golden vector");

    // The signature must bind the signed bytes: flip one tail byte -> different sig.
    uint8_t to_sign2[MESHCORE_MAX_PAYLOAD_SIZE];
    memcpy(to_sign2, to_sign, to_sign_len);
    to_sign2[to_sign_len - 1] ^= 0x01;
    uint8_t sig2[64];
    ed25519_sign(sig2, to_sign2, to_sign_len, pub, prv);
    CHECK(memcmp(sig, sig2, 64) != 0, "signature is bound to the signed bytes");

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_advert_sign: all checks passed\n");
    return 0;
}
