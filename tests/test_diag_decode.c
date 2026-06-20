// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// Host tests for the Toolbox packet-log decoder (diag_decode). Builds real
// on-air frames with the shipping serializers, then asserts diag_decode lifts
// the header + per-type fields the dissector renders. Keeps the decoder honest
// against any drift in the packet / advert wire layout.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "diag_decode.h"
#include "meshcore/packet.h"
#include "meshcore/payload/advert.h"

static int failures = 0;
#define CHECK(cond, msg)                                          \
    do {                                                          \
        if (!(cond)) { printf("FAIL: %s\n", (msg)); failures++; } \
    } while (0)

// Build a full frame from a message and decode it.
static bool decode_message(const meshcore_message_t *msg, diag_decoded_t *out) {
    uint8_t frame[MESHCORE_MAX_TRANS_UNIT] = {0};
    uint8_t flen                           = 0;
    if (meshcore_serialize(msg, frame, &flen) < 0) return false;
    return diag_decode(frame, flen, out);
}

static void test_advert_frame(void) {
    meshcore_advert_t adv = {0};
    for (int i = 0; i < MESHCORE_PUB_KEY_SIZE; i++) adv.pub_key[i] = (uint8_t)(0xA0 + i);
    adv.timestamp = 0x01020304u;
    for (int i = 0; i < MESHCORE_SIGNATURE_SIZE; i++) adv.signature[i] = (uint8_t)(i);
    adv.role           = MESHCORE_DEVICE_ROLE_REPEATER;
    adv.name_valid     = true;
    strcpy(adv.name, "repeater-south");
    adv.position_valid = true;
    adv.position_lat   = 52123456;
    adv.position_lon   = 4567890;

    meshcore_message_t msg = {0};
    msg.type           = MESHCORE_PAYLOAD_TYPE_ADVERT;
    msg.route          = MESHCORE_ROUTE_TYPE_FLOOD;
    msg.path_length    = 2;
    msg.bytes_per_hop  = 1;
    msg.path[0]        = 0x11;
    msg.path[1]        = 0x22;
    uint8_t psize      = 0;
    CHECK(meshcore_advert_serialize(&adv, msg.payload, &psize) >= 0, "advert payload serialize");
    msg.payload_length = psize;

    diag_decoded_t d = {0};
    CHECK(decode_message(&msg, &d), "advert frame decodes");
    CHECK(d.valid, "advert decoded valid");
    CHECK(d.ptype == MESHCORE_PAYLOAD_TYPE_ADVERT, "advert ptype");
    CHECK(d.route == MESHCORE_ROUTE_TYPE_FLOOD, "advert route");
    CHECK(d.hops == 2, "advert hop count = path_len / bytes_per_hop");
    CHECK(d.has_pubkey, "advert exposes pubkey");
    CHECK(memcmp(d.pubkey, adv.pub_key, MESHCORE_PUB_KEY_SIZE) == 0, "advert pubkey matches");
    CHECK(d.role == MESHCORE_DEVICE_ROLE_REPEATER, "advert role = repeater");
    CHECK(d.has_name && strcmp(d.name, "repeater-south") == 0, "advert name matches");
    CHECK(d.has_pos && d.lat_e6 == 52123456 && d.lon_e6 == 4567890, "advert position matches");
}

static void test_dm_frame(void) {
    meshcore_message_t msg = {0};
    msg.type           = MESHCORE_PAYLOAD_TYPE_TXT_MSG;
    msg.route          = MESHCORE_ROUTE_TYPE_DIRECT;
    msg.path_length    = 0;
    msg.bytes_per_hop  = 1;
    msg.payload[0]     = 0xDE;  // dest hash
    msg.payload[1]     = 0x5C;  // src hash
    msg.payload[2]     = 0xAA;  // ciphertext byte
    msg.payload_length = 18;    // 2 hashes + 16-byte cipher block

    diag_decoded_t d = {0};
    CHECK(decode_message(&msg, &d), "dm frame decodes");
    CHECK(d.ptype == MESHCORE_PAYLOAD_TYPE_TXT_MSG, "dm ptype");
    CHECK(d.route == MESHCORE_ROUTE_TYPE_DIRECT, "dm route = direct");
    CHECK(d.has_hash && d.dest_hash == 0xDE && d.src_hash == 0x5C, "dm dest/src hashes");
    CHECK(!d.has_pubkey, "dm has no advert pubkey");
}

static void test_truncated_dm(void) {
    // A TXT_MSG whose payload is too short to hold both 1-byte hashes must not
    // report dest/src — exercises the payload_length >= 2 guard.
    meshcore_message_t msg = {0};
    msg.type           = MESHCORE_PAYLOAD_TYPE_TXT_MSG;
    msg.route          = MESHCORE_ROUTE_TYPE_DIRECT;
    msg.bytes_per_hop  = 1;
    msg.payload[0]     = 0x42;
    msg.payload_length = 1;

    diag_decoded_t d = {0};
    CHECK(decode_message(&msg, &d), "truncated dm frame decodes");
    CHECK(d.valid, "truncated dm header parses");
    CHECK(d.ptype == MESHCORE_PAYLOAD_TYPE_TXT_MSG, "truncated dm ptype");
    CHECK(!d.has_hash, "truncated dm exposes no hashes (payload_length < 2)");
}

static void test_overlong_advert_name(void) {
    // Hand-build an ADVERT payload whose name exceeds MESHCORE_MAX_NAME_SIZE so
    // meshcore_advert_deserialize takes its reject path (advert.c name_len > 32).
    // diag_decode must keep the parsed header but lift no pubkey/name.
    uint8_t payload[160] = {0};
    int     p            = 0;
    for (int i = 0; i < MESHCORE_PUB_KEY_SIZE; i++) payload[p++] = (uint8_t)i;
    for (int i = 0; i < 4; i++) payload[p++] = 0;                  // timestamp
    for (int i = 0; i < MESHCORE_SIGNATURE_SIZE; i++) payload[p++] = 0;
    payload[p++] = 0x80;                                           // flags: name present
    for (int i = 0; i < 40; i++) payload[p++] = 'A';              // 40-char name (> 32)

    meshcore_message_t msg = {0};
    msg.type          = MESHCORE_PAYLOAD_TYPE_ADVERT;
    msg.route         = MESHCORE_ROUTE_TYPE_FLOOD;
    msg.bytes_per_hop = 1;
    memcpy(msg.payload, payload, p);
    msg.payload_length = (uint8_t)p;

    diag_decoded_t d = {0};
    CHECK(decode_message(&msg, &d), "overlong-name advert frame decodes");
    CHECK(d.valid, "overlong-name advert header still parses");
    CHECK(!d.has_pubkey, "overlong advert name rejected (no pubkey lifted)");
    CHECK(!d.has_name, "overlong advert name rejected (no name lifted)");
}

static void test_names_and_guards(void) {
    CHECK(strcmp(diag_type_name(MESHCORE_PAYLOAD_TYPE_ADVERT), "ADVERT") == 0, "type name ADVERT");
    CHECK(strcmp(diag_type_name(MESHCORE_PAYLOAD_TYPE_GRP_TXT), "CHAN") == 0, "type name CHAN");
    CHECK(strcmp(diag_route_name(MESHCORE_ROUTE_TYPE_DIRECT), "DIRECT") == 0, "route name DIRECT");
    CHECK(strcmp(diag_role_name(MESHCORE_DEVICE_ROLE_REPEATER), "Repeater") == 0, "role name Repeater");

    diag_decoded_t d;
    CHECK(!diag_decode(NULL, 0, &d), "decode rejects NULL frame");
    CHECK(!diag_decode((const uint8_t *)"\x00", 0, &d), "decode rejects zero length");
}

int main(void) {
    test_advert_frame();
    test_dm_frame();
    test_truncated_dm();
    test_overlong_advert_name();
    test_names_and_guards();
    if (failures == 0) {
        printf("test_diag_decode: OK\n");
        return 0;
    }
    printf("test_diag_decode: %d failure(s)\n", failures);
    return 1;
}
