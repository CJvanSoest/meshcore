// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
// SPDX-FileContributor: Ilias el Matani <hello@ilias.codes>
//
// Host integration test for a full public-channel (GRP_TXT) message lifecycle.
// It composes the two host-tested layers end to end: mc_crypto (channel
// encrypt/decrypt) and mc_proto (the GRP_TXT payload codec + the outer packet
// codec). A plaintext is framed, encrypted, serialized through both wire layers,
// then on the RX side deserialized and decrypted with the same key -- proving the
// codecs and the crypto agree on the byte layout. A wrong key must fail the MAC.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mc_crypto.h"
#include "meshcore/packet.h"
#include "meshcore/payload/grp_txt.h"

static int failures = 0;
#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (!(cond)) { printf("FAIL: %s\n", (msg)); failures++; }               \
    } while (0)

// Frame a GRP_TXT plaintext: timestamp(4) | text_type(1) | text, zero-padded to
// a 16-byte multiple. Returns the padded length.
static size_t frame_plain(uint8_t *plain, uint32_t ts, uint8_t text_type, const char *text) {
    memcpy(plain, &ts, 4);
    plain[4] = text_type;
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

// TX: frame -> encrypt -> grp_txt struct -> grp_txt_serialize -> message_t ->
// meshcore_serialize. Returns the on-wire packet in out_wire / out_wire_size.
static void tx_build(const uint8_t *key, uint32_t ts, uint8_t text_type,
                     const char *text, uint8_t channel_hash,
                     uint8_t *out_wire, uint8_t *out_wire_size) {
    uint8_t plain[256] = {0};
    size_t  padded     = frame_plain(plain, ts, text_type, text);

    uint8_t cipher[256] = {0};
    uint8_t mac[32];
    mc_crypto_grp_encrypt(key, plain, padded, cipher, mac);

    meshcore_grp_txt_t grp;
    memset(&grp, 0, sizeof grp);
    grp.channel_hash = channel_hash;
    memcpy(grp.mac, mac, MESHCORE_CIPHER_MAC_SIZE);
    grp.data_length = (uint8_t)padded;
    memcpy(grp.data, cipher, padded);

    uint8_t payload[MESHCORE_MAX_PAYLOAD_SIZE] = {0};
    uint8_t payload_size                       = 0;
    int     rc = meshcore_grp_txt_serialize(&grp, payload, &payload_size);
    CHECK(rc == 0, "grp_txt_serialize returns 0");

    meshcore_message_t msg;
    memset(&msg, 0, sizeof msg);
    msg.type           = MESHCORE_PAYLOAD_TYPE_GRP_TXT;
    msg.route          = MESHCORE_ROUTE_TYPE_FLOOD;
    msg.version        = 0;
    msg.path_length    = 0;
    msg.bytes_per_hop  = 1;
    msg.payload_length = payload_size;
    memcpy(msg.payload, payload, payload_size);

    rc = meshcore_serialize(&msg, out_wire, out_wire_size);
    CHECK(rc == 0, "meshcore_serialize returns 0");
}

// RX: meshcore_deserialize -> grp_txt_deserialize -> grp_decrypt with `key`.
// Returns the decrypt result; on success out_grp holds the decrypted struct.
static bool rx_decode(uint8_t *wire, uint8_t wire_size, const uint8_t *key,
                      meshcore_grp_txt_t *out_grp) {
    meshcore_message_t msg;
    int rc = meshcore_deserialize(wire, wire_size, &msg);
    CHECK(rc == 0, "meshcore_deserialize returns 0");
    CHECK(msg.type == MESHCORE_PAYLOAD_TYPE_GRP_TXT, "deserialized type is GRP_TXT");

    rc = meshcore_grp_txt_deserialize(msg.payload, msg.payload_length, out_grp);
    CHECK(rc == 0, "grp_txt_deserialize returns 0");

    return mc_crypto_grp_decrypt(out_grp, key);
}

static void test_full_lifecycle(void) {
    uint8_t key[MESHCORE_CIPHER_KEY_SIZE];
    fill_key(key, sizeof key, 0xA0);
    const uint32_t ts        = 0x5EED1234u;
    const uint8_t  text_type = 0;
    const char    *text      = "hello public channel";
    const uint8_t  chan_hash = 0x7C;

    uint8_t wire[MESHCORE_MAX_TRANS_UNIT] = {0};
    uint8_t wire_size                     = 0;
    tx_build(key, ts, text_type, text, chan_hash, wire, &wire_size);

    meshcore_grp_txt_t grp;
    bool ok = rx_decode(wire, wire_size, key, &grp);
    CHECK(ok, "end-to-end decrypt with the right key succeeds");
    CHECK(ok && grp.channel_hash == chan_hash, "channel_hash survives the round-trip");
    CHECK(ok && grp.decrypted.timestamp == ts, "recovered timestamp matches");
    CHECK(ok && grp.decrypted.text_type == text_type, "recovered text_type matches");
    CHECK(ok && strcmp(grp.decrypted.text, text) == 0, "recovered text equals the original");
}

static void test_wrong_key_fails(void) {
    uint8_t key[MESHCORE_CIPHER_KEY_SIZE], wrong[MESHCORE_CIPHER_KEY_SIZE];
    fill_key(key, sizeof key, 0xA0);
    fill_key(wrong, sizeof wrong, 0x10);

    uint8_t wire[MESHCORE_MAX_TRANS_UNIT] = {0};
    uint8_t wire_size                     = 0;
    tx_build(key, 0x11223344u, 0, "secret on the wire", 0x7C, wire, &wire_size);

    meshcore_grp_txt_t grp;
    bool ok = rx_decode(wire, wire_size, wrong, &grp);
    CHECK(!ok, "end-to-end decrypt with a wrong key fails the MAC");
}

int main(void) {
    test_full_lifecycle();
    test_wrong_key_fails();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_integration_message: all checks passed\n");
    return 0;
}
