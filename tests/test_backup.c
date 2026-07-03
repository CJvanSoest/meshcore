// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// Host test for the on-SD config backup codec (backup_codec.c). Guards the
// serialise/parse round-trip plus every rejection path (magic, version, CRC,
// range/truncation) — the backup file is what restores private-channel secrets
// after an NVS wipe, so a silent codec bug would defeat the whole safety net.

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "backup_codec.h"

static int failures = 0;
#define CHECK(cond, msg)                                             \
    do {                                                             \
        if (!(cond)) {                                               \
            printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            failures++;                                              \
        }                                                            \
    } while (0)

static backup_data_t sample(void) {
    backup_data_t d;
    memset(&d, 0, sizeof(d));
    d.n_channels = 2;
    strcpy(d.channels[0].name, "#nl");
    for (int i = 0; i < BACKUP_SECRET_LEN; i++) d.channels[0].secret[i] = (uint8_t)(0x10 + i);
    strcpy(d.channels[1].name, "secret-room");
    for (int i = 0; i < BACKUP_SECRET_LEN; i++)
        d.channels[1].secret[i] = (uint8_t)(0xA0 + i);  // "private" random-ish key
    d.n_contacts = 1;
    for (int i = 0; i < BACKUP_PUB_LEN; i++) d.contacts[0].pub[i] = (uint8_t)(i * 3 + 1);
    strcpy(d.contacts[0].alias, "Alice");
    d.contacts[0].role = 2;
    // version-2 tail
    d.have_identity    = 1;
    for (int i = 0; i < BACKUP_SEED_LEN; i++) d.identity_seed[i] = (uint8_t)(0x5A ^ i);
    d.radio_len = 12;
    for (int i = 0; i < d.radio_len; i++) d.radio[i] = (uint8_t)(i + 100);
    d.have_location = 1;
    d.lat_e6        = 52371234;
    d.lon_e6        = 4894560;
    return d;
}

static void test_roundtrip(void) {
    printf("test_roundtrip\n");
    backup_data_t d = sample();
    uint8_t       buf[BACKUP_MAX_BYTES];
    size_t        n = backup_encode(&d, buf, sizeof(buf));
    CHECK(n > 0, "encode returns non-zero length");
    CHECK(memcmp(buf, BACKUP_MAGIC, 4) == 0, "magic present");

    backup_data_t got;
    int           rc = backup_decode(buf, n, &got);
    CHECK(rc == BACKUP_OK, "decode ok");
    CHECK(got.n_channels == 2, "channel count preserved");
    CHECK(got.n_contacts == 1, "contact count preserved");
    CHECK(strcmp(got.channels[0].name, "#nl") == 0, "ch0 name");
    CHECK(strcmp(got.channels[1].name, "secret-room") == 0, "ch1 name");
    CHECK(memcmp(got.channels[1].secret, d.channels[1].secret, BACKUP_SECRET_LEN) == 0,
          "private secret survives byte-exact");
    CHECK(memcmp(got.contacts[0].pub, d.contacts[0].pub, BACKUP_PUB_LEN) == 0, "contact pub survives");
    CHECK(strcmp(got.contacts[0].alias, "Alice") == 0, "contact alias");
    CHECK(got.contacts[0].role == 2, "contact role");
    CHECK(got.have_identity == 1, "identity flag");
    CHECK(memcmp(got.identity_seed, d.identity_seed, BACKUP_SEED_LEN) == 0, "identity seed byte-exact");
    CHECK(got.radio_len == 12 && memcmp(got.radio, d.radio, 12) == 0, "radio blob survives");
    CHECK(got.have_location == 1 && got.lat_e6 == 52371234 && got.lon_e6 == 4894560, "location survives");
}

// A version-1 file (channels + contacts, no tail) must still decode, with the
// v2 tail fields left zeroed — so an existing backup keeps working after the
// format bump.
static void test_v1_compat(void) {
    printf("test_v1_compat\n");
    uint8_t buf[128];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, BACKUP_MAGIC, 4);
    buf[4]   = 1;  // version 1
    size_t o = BACKUP_HDR_LEN;
    buf[o++] = 1;  // 1 channel
    memcpy(buf + o, "#v1", 3);
    o += BACKUP_CH_NAME_LEN;
    for (int i = 0; i < BACKUP_SECRET_LEN; i++) buf[o + i] = (uint8_t)i;
    o            += BACKUP_SECRET_LEN;
    buf[o++]      = 0;  // 0 contacts
    uint32_t crc  = backup_crc32(buf + BACKUP_HDR_LEN, o - BACKUP_HDR_LEN);
    buf[8]        = (uint8_t)(crc & 0xFF);
    buf[9]        = (uint8_t)((crc >> 8) & 0xFF);
    buf[10]       = (uint8_t)((crc >> 16) & 0xFF);
    buf[11]       = (uint8_t)((crc >> 24) & 0xFF);
    backup_data_t got;
    CHECK(backup_decode(buf, o, &got) == BACKUP_OK, "v1 decodes");
    CHECK(got.n_channels == 1 && strcmp(got.channels[0].name, "#v1") == 0, "v1 channel parsed");
    CHECK(got.have_identity == 0 && got.radio_len == 0 && got.have_location == 0, "v1 tail zeroed");
}

static void test_empty(void) {
    printf("test_empty\n");
    backup_data_t d;
    memset(&d, 0, sizeof(d));
    uint8_t buf[BACKUP_MAX_BYTES];
    size_t  n = backup_encode(&d, buf, sizeof(buf));
    CHECK(n > 0, "empty encodes");
    backup_data_t got;
    CHECK(backup_decode(buf, n, &got) == BACKUP_OK, "empty decodes");
    CHECK(got.n_channels == 0 && got.n_contacts == 0, "empty counts");
}

static void test_full(void) {
    printf("test_full\n");
    backup_data_t d;
    memset(&d, 0, sizeof(d));
    d.n_channels = BACKUP_MAX_CHANNELS;
    d.n_contacts = BACKUP_MAX_CONTACTS;
    for (int i = 0; i < BACKUP_MAX_CHANNELS; i++) d.channels[i].secret[0] = (uint8_t)i;
    uint8_t buf[BACKUP_MAX_BYTES];
    size_t  n = backup_encode(&d, buf, sizeof(buf));
    CHECK(n > 0 && n <= BACKUP_MAX_BYTES, "max payload fits in BACKUP_MAX_BYTES");
    backup_data_t got;
    CHECK(backup_decode(buf, n, &got) == BACKUP_OK, "max decodes");
    CHECK(got.n_channels == BACKUP_MAX_CHANNELS && got.n_contacts == BACKUP_MAX_CONTACTS, "max counts");
}

static void test_reject_crc(void) {
    printf("test_reject_crc\n");
    backup_data_t d = sample();
    uint8_t       buf[BACKUP_MAX_BYTES];
    size_t        n          = backup_encode(&d, buf, sizeof(buf));
    buf[BACKUP_HDR_LEN + 3] ^= 0xFF;  // flip a body byte, CRC no longer matches
    backup_data_t got;
    CHECK(backup_decode(buf, n, &got) == BACKUP_ERR_CRC, "corrupt body rejected via CRC");
}

static void test_reject_magic(void) {
    printf("test_reject_magic\n");
    backup_data_t d = sample();
    uint8_t       buf[BACKUP_MAX_BYTES];
    size_t        n = backup_encode(&d, buf, sizeof(buf));
    buf[0]          = 'X';
    backup_data_t got;
    CHECK(backup_decode(buf, n, &got) == BACKUP_ERR_MAGIC, "bad magic rejected");
}

static void test_reject_version(void) {
    printf("test_reject_version\n");
    backup_data_t d = sample();
    uint8_t       buf[BACKUP_MAX_BYTES];
    size_t        n = backup_encode(&d, buf, sizeof(buf));
    buf[4]          = 99;  // bump version; CRC still covers only body so magic/version checks run first
    backup_data_t got;
    CHECK(backup_decode(buf, n, &got) == BACKUP_ERR_VERSION, "unknown version rejected");
}

static void test_reject_truncation(void) {
    printf("test_reject_truncation\n");
    backup_data_t d = sample();
    uint8_t       buf[BACKUP_MAX_BYTES];
    size_t        n = backup_encode(&d, buf, sizeof(buf));
    // Chop the last few bytes: CRC recomputed over the shorter buffer won't
    // match the stored value, so this trips CRC (a hard-truncated file is
    // never silently half-restored).
    backup_data_t got;
    CHECK(backup_decode(buf, n - 5, &got) != BACKUP_OK, "truncated buffer rejected");
    CHECK(backup_decode(buf, 3, &got) == BACKUP_ERR_SHORT, "tiny buffer rejected as short");
}

static void test_reject_range(void) {
    printf("test_reject_range\n");
    // Hand-build a header claiming more channels than the body carries, with a
    // valid CRC over that body, to hit the range/truncation guard directly.
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, BACKUP_MAGIC, 4);
    buf[4]                  = BACKUP_VERSION;
    size_t body_len         = 2;  // just the two count bytes
    buf[BACKUP_HDR_LEN]     = 5;  // claims 5 channels but body has none
    buf[BACKUP_HDR_LEN + 1] = 0;
    uint32_t crc            = backup_crc32(buf + BACKUP_HDR_LEN, body_len);
    buf[8]                  = (uint8_t)(crc & 0xFF);
    buf[9]                  = (uint8_t)((crc >> 8) & 0xFF);
    buf[10]                 = (uint8_t)((crc >> 16) & 0xFF);
    buf[11]                 = (uint8_t)((crc >> 24) & 0xFF);
    backup_data_t got;
    CHECK(backup_decode(buf, BACKUP_HDR_LEN + body_len, &got) == BACKUP_ERR_RANGE, "over-declared count rejected");
}

int main(void) {
    printf("=== test_backup ===\n");
    test_roundtrip();
    test_v1_compat();
    test_empty();
    test_full();
    test_reject_crc();
    test_reject_magic();
    test_reject_version();
    test_reject_truncation();
    test_reject_range();
    if (failures) {
        printf("FAILED: %d check(s)\n", failures);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
