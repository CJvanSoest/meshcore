// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
// SPDX-FileContributor: Ilias el Matani <hello@ilias.codes>
//
// Host tests for the GRP_TXT and ADVERT payload codecs. packet.c (the outer
// frame) already has test_meshcore_packet; these cover the two payload bodies
// that ride inside it, which had no host coverage. Round-trip each through
// serialize -> deserialize and assert every field survives, plus a couple of
// flag/absent-field cases.

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "meshcore/payload/advert.h"
#include "meshcore/payload/grp_txt.h"

static int failures = 0;
#define CHECK(cond, msg)                 \
    do {                                 \
        if (!(cond)) {                   \
            printf("FAIL: %s\n", (msg)); \
            failures++;                  \
        }                                \
    } while (0)

static void test_grp_txt_roundtrip(void) {
    meshcore_grp_txt_t in = {0};
    in.channel_hash       = 0x7A;
    for (int i = 0; i < MESHCORE_CIPHER_MAC_SIZE; i++) in.mac[i] = (uint8_t)(0x10 + i);
    in.data_length = 32;
    for (int i = 0; i < 32; i++) in.data[i] = (uint8_t)(i * 7);

    uint8_t payload[256] = {0};
    uint8_t size         = 0;
    CHECK(meshcore_grp_txt_serialize(&in, payload, &size) >= 0, "grp_txt serialize");

    meshcore_grp_txt_t out = {0};
    CHECK(meshcore_grp_txt_deserialize(payload, size, &out) >= 0, "grp_txt deserialize");
    CHECK(out.channel_hash == in.channel_hash, "grp_txt channel_hash survives");
    CHECK(memcmp(out.mac, in.mac, MESHCORE_CIPHER_MAC_SIZE) == 0, "grp_txt mac survives");
    CHECK(out.data_length == in.data_length, "grp_txt data_length survives");
    CHECK(memcmp(out.data, in.data, in.data_length) == 0, "grp_txt ciphertext survives");
}

static void test_advert_roundtrip(void) {
    meshcore_advert_t in = {0};
    for (int i = 0; i < MESHCORE_PUB_KEY_SIZE; i++) in.pub_key[i] = (uint8_t)i;
    in.timestamp = 0x01020304u;
    for (int i = 0; i < MESHCORE_SIGNATURE_SIZE; i++) in.signature[i] = (uint8_t)(255 - i);
    in.role       = MESHCORE_DEVICE_ROLE_CHAT_NODE;
    in.name_valid = true;
    strcpy(in.name, "tanmatsu");
    in.position_valid = true;
    in.position_lat   = 52123456;
    in.position_lon   = 4567890;
    in.extra1_valid   = true;
    in.extra1         = 0xBEEF;
    // extra2 deliberately absent

    uint8_t payload[256] = {0};
    uint8_t size         = 0;
    CHECK(meshcore_advert_serialize(&in, payload, &size) >= 0, "advert serialize");

    // Wire layout golden: the flags byte sits right after the 64-byte signature
    // and carries position|extra1|name (0x10|0x20|0x80) with role 0 in the low
    // nibble. Guards against serialize/deserialize drifting together.
    CHECK(payload[MESHCORE_PUB_KEY_SIZE + 4 + MESHCORE_SIGNATURE_SIZE] ==
              (uint8_t)((in.role & 0x0F) | 0x10 | 0x20 | 0x80),
          "advert flags byte at the right offset with the right bits");

    meshcore_advert_t out = {0};
    CHECK(meshcore_advert_deserialize(payload, size, &out) >= 0, "advert deserialize");
    CHECK(memcmp(out.pub_key, in.pub_key, MESHCORE_PUB_KEY_SIZE) == 0, "advert pub_key survives");
    CHECK(out.timestamp == in.timestamp, "advert timestamp survives");
    CHECK(memcmp(out.signature, in.signature, MESHCORE_SIGNATURE_SIZE) == 0, "advert signature survives");
    CHECK(out.role == in.role, "advert role survives");
    CHECK(out.name_valid && strcmp(out.name, in.name) == 0, "advert name survives");
    CHECK(out.position_valid && out.position_lat == in.position_lat && out.position_lon == in.position_lon,
          "advert position survives");
    CHECK(out.extra1_valid && out.extra1 == in.extra1, "advert extra1 survives");
    CHECK(!out.extra2_valid, "advert extra2 stays absent");
}

// ADVERT deserialize is the attack surface: it runs on every advert from an
// untrusted radio peer. Exercise the malformed/optional paths the round-trip
// never reaches. Flag bits are local to advert.c (0x10 position, 0x80 name).
static void test_advert_rejects_and_optional(void) {
    meshcore_advert_t in = {0};
    for (int i = 0; i < MESHCORE_PUB_KEY_SIZE; i++) in.pub_key[i] = (uint8_t)i;
    in.timestamp      = 0x11223344u;
    in.role           = MESHCORE_DEVICE_ROLE_CHAT_NODE;
    uint8_t base[256] = {0}, blen = 0;
    meshcore_advert_serialize(&in, base, &blen);  // no optional fields set

    const uint8_t     prefix = MESHCORE_PUB_KEY_SIZE + 4 + MESHCORE_SIGNATURE_SIZE;
    meshcore_advert_t out    = {0};

    // Too short to even hold pub_key + timestamp + signature.
    CHECK(meshcore_advert_deserialize(base, (uint8_t)(prefix - 1), &out) < 0, "advert rejects a sub-header buffer");

    // Bare advert: exactly the prefix, no flags byte -> valid, no optionals.
    CHECK(meshcore_advert_deserialize(base, prefix, &out) >= 0, "bare advert is valid");
    CHECK(!out.name_valid && !out.position_valid && !out.extra1_valid && !out.extra2_valid,
          "bare advert carries no optional fields");

    // Position flag set but the 8 coordinate bytes are missing -> reject.
    uint8_t p[256];
    memcpy(p, base, prefix);
    p[prefix] = 0x10;  // ADVERT_FLAG_HAS_POSITION, nothing after it
    CHECK(meshcore_advert_deserialize(p, (uint8_t)(prefix + 1), &out) < 0,
          "advert rejects the position flag with no coordinates");

    // Name exactly at the max -> accepted and NUL terminated; one over -> reject.
    uint8_t n[256];
    memcpy(n, base, prefix);
    n[prefix] = 0x80;  // ADVERT_FLAG_NAME
    for (int i = 0; i < MESHCORE_MAX_NAME_SIZE + 1; i++) n[prefix + 1 + i] = 'A';
    CHECK(meshcore_advert_deserialize(n, (uint8_t)(prefix + 1 + MESHCORE_MAX_NAME_SIZE), &out) >= 0 && out.name_valid &&
              (int)strlen(out.name) == MESHCORE_MAX_NAME_SIZE,
          "advert accepts a max-length name and NUL terminates it");
    CHECK(meshcore_advert_deserialize(n, (uint8_t)(prefix + 1 + MESHCORE_MAX_NAME_SIZE + 1), &out) < 0,
          "advert rejects a name over the max length");
}

// GRP_TXT deserialize runs on every received channel packet; its two bounds
// checks (min size, oversized data_length) had no coverage.
static void test_grp_txt_rejects(void) {
    meshcore_grp_txt_t out      = {0};
    uint8_t            buf[256] = {0};
    CHECK(meshcore_grp_txt_deserialize(buf, MESHCORE_CIPHER_MAC_SIZE, &out) < 0,
          "grp_txt rejects a frame shorter than channel_hash + mac");
    CHECK(meshcore_grp_txt_deserialize(buf, (uint8_t)(1 + MESHCORE_CIPHER_MAC_SIZE + 200), &out) < 0,
          "grp_txt rejects an oversized data_length");
}

int main(void) {
    test_grp_txt_roundtrip();
    test_advert_roundtrip();
    test_advert_rejects_and_optional();
    test_grp_txt_rejects();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_meshcore_payloads: all checks passed\n");
    return 0;
}
