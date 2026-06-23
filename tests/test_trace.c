// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// Host tests for the TRACE payload layout (the coverage-test reachability
// probe). Locks the byte order against upstream (tag | auth | flags | hashes)
// with a golden vector plus a build/parse round-trip.

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "trace.h"

static int failures = 0;
#define CHECK(cond, msg)                 \
    do {                                 \
        if (!(cond)) {                   \
            printf("FAIL: %s\n", (msg)); \
            failures++;                  \
        }                                \
    } while (0)

static void test_golden_layout(void) {
    // tag = 0x04030201, auth = 0x08070605, path_sz = 1, one 2-byte hop hash.
    uint8_t hashes[2] = {0xAA, 0xBB};
    uint8_t out[16]   = {0};
    uint8_t n         = meshcore_trace_build_payload(0x04030201u, 0x08070605u, 1, hashes, 2, out);

    CHECK(n == MESHCORE_TRACE_HDR_LEN + 2, "payload length = 9 + hashes_len");
    // Little-endian tag + auth, then flags, then the hash bytes.
    uint8_t golden[11] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x01, 0xAA, 0xBB};
    CHECK(memcmp(out, golden, 11) == 0, "golden TRACE payload bytes");
    CHECK((out[8] & 0x03) == 1, "flags low 2 bits carry path_sz");
}

static void test_roundtrip(void) {
    uint8_t hashes[4] = {0x11, 0x22, 0x33, 0x44};
    uint8_t out[32]   = {0};
    uint8_t n         = meshcore_trace_build_payload(0xDEADBEEFu, 0, 2, hashes, 4, out);

    uint32_t       tag = 0, auth = 9;
    uint8_t        flags = 0, hlen = 0;
    const uint8_t* hp = NULL;
    CHECK(meshcore_trace_parse(out, n, &tag, &auth, &flags, &hp, &hlen), "parse ok");
    CHECK(tag == 0xDEADBEEFu, "tag round-trips");
    CHECK(auth == 0, "auth round-trips");
    CHECK((flags & 0x03) == 2, "path_sz round-trips");
    CHECK(hlen == 4 && hp != NULL && memcmp(hp, hashes, 4) == 0, "hashes round-trip");
}

static void test_guards(void) {
    uint8_t out[16];
    CHECK(meshcore_trace_build_payload(1, 2, 0, NULL, 3, out) == 0, "reject hashes==NULL with len>0");
    CHECK(meshcore_trace_build_payload(1, 2, 0, NULL, 0, NULL) == 0, "reject out==NULL");

    uint8_t short_buf[4] = {0};
    CHECK(!meshcore_trace_parse(short_buf, 4, NULL, NULL, NULL, NULL, NULL), "reject truncated payload");

    CHECK(meshcore_trace_path_sz(1) == 0, "path_sz(1 byte) = 0");
    CHECK(meshcore_trace_path_sz(2) == 1, "path_sz(2 byte) = 1");
    CHECK(meshcore_trace_path_sz(4) == 2, "path_sz(4 byte) = 2");
}

int main(void) {
    test_golden_layout();
    test_roundtrip();
    test_guards();
    if (failures == 0) {
        printf("test_trace: OK\n");
        return 0;
    }
    printf("test_trace: %d failure(s)\n", failures);
    return 1;
}
