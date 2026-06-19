// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// Host-side test for the MeshCore packet codec in main/meshcore/packet.c.
//
// This is the wire boundary that has cost the most real-user pain in this
// project (see docs/ARCHITECTURE.md "Wire-boundary discipline"). The test links
// against the SAME translation unit the firmware ships and round-trips a
// message through serialize → deserialize, so a regression in the header
// bit-packing, the transport-code gating, or the path length-byte encoding
// makes CI red before it reaches a node on the air.
//
// Build (see tests/Makefile):
//     gcc -I../main test_meshcore_packet.c ../main/meshcore/packet.c -o test_meshcore_packet
//
// Exit 0 on pass, 1 on any mismatch.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "meshcore/packet.h"

static int failures = 0;

#define EXPECT(cond, fmt, ...) do {                                            \
    if (!(cond)) {                                                             \
        fprintf(stderr, "FAIL %s:%d: " fmt "\n", __FILE__, __LINE__,           \
                ##__VA_ARGS__);                                                \
        failures++;                                                            \
    }                                                                          \
} while (0)

int main(void) {
    uint8_t buf[MESHCORE_MAX_TRANS_UNIT];
    uint8_t size = 0;

    // ── TV1: FLOOD text message, no path. Smallest meaningful packet:
    //         header byte + path-length byte + payload. ─────────────────────
    {
        meshcore_message_t m;
        memset(&m, 0, sizeof(m));
        m.type    = MESHCORE_PAYLOAD_TYPE_TXT_MSG;
        m.route   = MESHCORE_ROUTE_TYPE_FLOOD;
        m.version = 1;
        const char *pl = "hello mesh";
        m.payload_length = (uint8_t)strlen(pl);
        memcpy(m.payload, pl, m.payload_length);

        EXPECT(meshcore_serialize(&m, buf, &size) == 0, "TV1 serialize ok");
        EXPECT(size == 1 + 1 + 10, "TV1 size = header+plb+payload, got %u", size);

        meshcore_message_t d;
        EXPECT(meshcore_deserialize(buf, size, &d) == 0, "TV1 deserialize ok");
        EXPECT(d.type == MESHCORE_PAYLOAD_TYPE_TXT_MSG, "TV1 type");
        EXPECT(d.route == MESHCORE_ROUTE_TYPE_FLOOD, "TV1 route");
        EXPECT(d.version == 1, "TV1 version");
        EXPECT(d.path_length == 0, "TV1 no path");
        EXPECT(d.bytes_per_hop == 1, "TV1 empty path normalises bph to 1");
        EXPECT(d.payload_length == 10, "TV1 payload_length, got %u", d.payload_length);
        EXPECT(memcmp(d.payload, pl, 10) == 0, "TV1 payload bytes survive");
    }

    // ── TV2: TRANSPORT_FLOOD advert with two transport codes and a 2-byte-
    //         per-hop, 2-hop path. Exercises the transport-code gating and the
    //         (bph-1)<<6 | hop_count length-byte encoding. ──────────────────
    {
        meshcore_message_t m;
        memset(&m, 0, sizeof(m));
        m.type               = MESHCORE_PAYLOAD_TYPE_ADVERT;
        m.route              = MESHCORE_ROUTE_TYPE_TRANSPORT_FLOOD;
        m.version            = 1;
        m.transport_codes[0] = 0x1234;
        m.transport_codes[1] = 0x5678;
        m.bytes_per_hop      = 2;
        m.path_length        = 4;  // 2 hops * 2 bytes
        uint8_t path[4]      = {0xAA, 0xBB, 0xCC, 0xDD};
        memcpy(m.path, path, 4);
        uint8_t pay[3]       = {0x01, 0x02, 0x03};
        m.payload_length     = 3;
        memcpy(m.payload, pay, 3);

        EXPECT(meshcore_serialize(&m, buf, &size) == 0, "TV2 serialize ok");
        EXPECT(size == 1 + 4 + 1 + 4 + 3,
               "TV2 size = header+codes+plb+path+payload, got %u", size);

        meshcore_message_t d;
        EXPECT(meshcore_deserialize(buf, size, &d) == 0, "TV2 deserialize ok");
        EXPECT(d.route == MESHCORE_ROUTE_TYPE_TRANSPORT_FLOOD, "TV2 route");
        EXPECT(d.type == MESHCORE_PAYLOAD_TYPE_ADVERT, "TV2 type");
        EXPECT(d.transport_codes[0] == 0x1234 && d.transport_codes[1] == 0x5678,
               "TV2 transport codes survive");
        EXPECT(d.bytes_per_hop == 2, "TV2 bytes_per_hop, got %u", d.bytes_per_hop);
        EXPECT(d.path_length == 4, "TV2 path_length, got %u", d.path_length);
        EXPECT(memcmp(d.path, path, 4) == 0, "TV2 path bytes survive");
        EXPECT(d.payload_length == 3 && memcmp(d.payload, pay, 3) == 0,
               "TV2 payload survives");
    }

    // ── TV3: bytes_per_hop == 0 from the caller means "use 1" on the wire,
    //         and deserialize reports it back as 1. ──────────────────────────
    {
        meshcore_message_t m;
        memset(&m, 0, sizeof(m));
        m.type           = MESHCORE_PAYLOAD_TYPE_TXT_MSG;
        m.route          = MESHCORE_ROUTE_TYPE_FLOOD;
        m.bytes_per_hop  = 0;   // caller leaves it unset
        m.path_length    = 3;
        uint8_t path[3]  = {0x10, 0x20, 0x30};
        memcpy(m.path, path, 3);

        EXPECT(meshcore_serialize(&m, buf, &size) == 0, "TV3 serialize ok");
        meshcore_message_t d;
        EXPECT(meshcore_deserialize(buf, size, &d) == 0, "TV3 deserialize ok");
        EXPECT(d.bytes_per_hop == 1, "TV3 bph 0 normalises to 1");
        EXPECT(d.path_length == 3 && memcmp(d.path, path, 3) == 0, "TV3 path survives");
    }

    // ── TV4: error paths must be rejected, not silently mangled. ─────────────
    {
        meshcore_message_t d;
        EXPECT(meshcore_deserialize(buf, 0, &d) == -1, "size 0 rejected (< header)");
        EXPECT(meshcore_deserialize(NULL, 8, &d) == -1, "NULL data rejected");

        // Header claims TRANSPORT_FLOOD but the buffer carries no transport codes.
        uint8_t trunc = (uint8_t)((MESHCORE_ROUTE_TYPE_TRANSPORT_FLOOD & 0x03) |
                                  ((MESHCORE_PAYLOAD_TYPE_ADVERT & 0x0F) << 2));
        EXPECT(meshcore_deserialize(&trunc, 1, &d) == -1,
               "truncated transport header rejected");

        meshcore_message_t m;
        memset(&m, 0, sizeof(m));
        m.route         = MESHCORE_ROUTE_TYPE_FLOOD;
        m.bytes_per_hop = 2;
        m.path_length   = 5;  // not a multiple of 2
        EXPECT(meshcore_serialize(&m, buf, &size) == -1,
               "path_length not divisible by bytes_per_hop rejected");

        memset(&m, 0, sizeof(m));
        m.payload_length = MESHCORE_MAX_PAYLOAD_SIZE + 1;
        EXPECT(meshcore_serialize(&m, buf, &size) == -1, "oversized payload rejected");
    }

    if (failures == 0) {
        printf("OK -- all MeshCore packet codec vectors passed\n");
        return 0;
    }
    fprintf(stderr, "%d test vector failure(s)\n", failures);
    return 1;
}
