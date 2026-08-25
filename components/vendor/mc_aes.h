// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// Thin AES-128 wrapper over the vendored public-domain tiny-AES (aes/). Gives
// the small set of block operations the MeshCore crypto uses (single-block ECB
// + CBC) with a mbedtls-shaped call pattern, so the call sites read the same
// after dropping mbedtls/aes.h — which mbedtls 4.1 (ESP-IDF 6) made private.
// Software AES; fine for MeshCore's low crypto volume. Validated against the
// NIST SP800-38A vectors in tests/test_mc_aes.c.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "aes/aes.h"

typedef struct {
    struct AES_ctx ctx;
} mc_aes_t;

// Set the 128-bit key. Reusable for both directions and any number of blocks.
void mc_aes128_init(mc_aes_t* a, const uint8_t key[16]);

// Single 16-byte block, ECB. in and out may alias.
void mc_aes128_ecb_encrypt(mc_aes_t* a, const uint8_t in[16], uint8_t out[16]);
void mc_aes128_ecb_decrypt(mc_aes_t* a, const uint8_t in[16], uint8_t out[16]);

// CBC over len bytes (len must be a multiple of 16). iv is read-only here (the
// callers keep their own IV); out and in may alias.
void mc_aes128_cbc_encrypt(mc_aes_t* a, const uint8_t iv[16], size_t len, const uint8_t* in, uint8_t* out);
void mc_aes128_cbc_decrypt(mc_aes_t* a, const uint8_t iv[16], size_t len, const uint8_t* in, uint8_t* out);
