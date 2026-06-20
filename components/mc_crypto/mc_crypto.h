// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
// SPDX-FileContributor: Ilias el Matani <hello@ilias.codes>
//
// Pure MeshCore symmetric crypto, lifted out of radio.c so the security-
// critical channel-decrypt, channel-encrypt and ACK-binding paths can be unit
// tested on the host. mbedtls only -- no ESP-IDF, no radio or domain state.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "meshcore/packet.h"
#include "meshcore/payload/grp_txt.h"

// Channel (GRP_TXT) decrypt. Verify HMAC-SHA256(key) over the on-wire
// ciphertext (grp->data), then AES-128-ECB decrypt into grp->decrypted and
// parse timestamp|text_type|text. Returns false and writes no plaintext when
// the MAC does not match -- this is the channel-match test: a wrong key just
// fails the MAC.
bool mc_crypto_grp_decrypt(meshcore_grp_txt_t* grp, const uint8_t* key);

// Channel (GRP_TXT) encrypt. AES-128-ECB encrypt `plain` (already framed and
// zero-padded to a 16-byte multiple) under `key` into out_cipher, and write
// HMAC-SHA256(key, cipher) into out_mac (full 32 bytes; callers keep the first
// MESHCORE_CIPHER_MAC_SIZE).
void mc_crypto_grp_encrypt(const uint8_t* key, const uint8_t* plain, size_t padded_len, uint8_t* out_cipher,
                           uint8_t out_mac[32]);

// ACK-binding CRC = SHA256(head5 || text || pubkey)[0:4], where head5 is the DM
// plaintext prefix (timestamp[4] | flags[1]). The DM sender computes it to
// predict the ACK; the receiver computes it to build the PATH_RETURN. Both must
// agree byte-for-byte.
void mc_crypto_ack_crc(const uint8_t head5[5], const char* text, size_t text_len,
                       const uint8_t pubkey[MESHCORE_PUB_KEY_SIZE], uint8_t out_crc[4]);

// Direct-message (TXT_MSG) encrypt: ed25519 ECDH(target_pub, my_prv) shared
// secret, AES-128-ECB encrypt `plain` (zero-padded to a 16-byte multiple) into
// out_cipher, and HMAC-SHA256(shared, cipher) into out_mac (32 bytes; the wire
// keeps the first two). my_prv is the 64-byte ed25519 private key.
void mc_crypto_dm_encrypt(const uint8_t target_pub[MESHCORE_PUB_KEY_SIZE], const uint8_t* my_prv, const uint8_t* plain,
                          size_t padded_len, uint8_t* out_cipher, uint8_t out_mac[32]);

// Direct-message decrypt of an on-wire TXT_MSG payload (dst[1] src[1] mac[2]
// cipher[...]). Tries the four candidate shared secrets (ed25519 conv/raw x
// 16/32-byte HMAC key), accepts on the first whose HMAC[0..1] matches the wire
// mac, then AES-128-ECB decrypts into out_plaintext. Returns false for a wrong
// key. out_good_secret receives the 32-byte secret that verified.
bool mc_crypto_dm_decrypt(const uint8_t* payload, uint8_t payload_len, const uint8_t sender_pub[MESHCORE_PUB_KEY_SIZE],
                          const uint8_t* my_prv, uint8_t* out_plaintext, int* out_text_len,
                          uint8_t out_good_secret[32]);

// Region-scope transport code = HMAC-SHA256(SHA256("#"||region)[0:16],
// type || payload)[0:2], with 0x0000/0xFFFF remapped to 0x0001/0xFFFE. The
// '#' prefix is prepended if absent (upstream RegionMap convention); pass the
// raw region name either way. Used to tag ROUTE_TYPE_TRANSPORT_FLOOD packets
// so scope-aware relays route them.
uint16_t mc_crypto_region_transport_code(const char* region_name, uint8_t type, const uint8_t* payload,
                                         uint16_t payload_len);
