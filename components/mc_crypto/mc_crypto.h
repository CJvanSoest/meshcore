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
bool mc_crypto_grp_decrypt(meshcore_grp_txt_t *grp, const uint8_t *key);

// Channel (GRP_TXT) encrypt. AES-128-ECB encrypt `plain` (already framed and
// zero-padded to a 16-byte multiple) under `key` into out_cipher, and write
// HMAC-SHA256(key, cipher) into out_mac (full 32 bytes; callers keep the first
// MESHCORE_CIPHER_MAC_SIZE).
void mc_crypto_grp_encrypt(const uint8_t *key, const uint8_t *plain,
                           size_t padded_len, uint8_t *out_cipher,
                           uint8_t out_mac[32]);

// ACK-binding CRC = SHA256(head5 || text || pubkey)[0:4], where head5 is the DM
// plaintext prefix (timestamp[4] | flags[1]). The DM sender computes it to
// predict the ACK; the receiver computes it to build the PATH_RETURN. Both must
// agree byte-for-byte.
void mc_crypto_ack_crc(const uint8_t head5[5], const char *text, size_t text_len,
                       const uint8_t pubkey[MESHCORE_PUB_KEY_SIZE],
                       uint8_t out_crc[4]);
