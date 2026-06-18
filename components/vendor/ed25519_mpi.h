// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// Ed25519 sign/keypair via mbedtls_mpi -- correctness > speed.
//
// Replaces the broken ref10-style implementation that was previously in
// ed25519.c (produced wrong points for the RFC 8032 test vector, silently
// rejected by every upstream MeshCore verifier). All field/group arithmetic
// goes through mbedtls_mpi, the same well-tested BigInt layer that already
// powers our working X25519 path.
//
// Depends ONLY on mbedtls (sha512 + bignum) -- no esp-idf -- so the same code
// is host-buildable and exercised by tests/test_ed25519.c in CI before merge.

#pragma once

#include <stdint.h>
#include <stddef.h>

void ed25519_create_keypair(uint8_t *public_key,
                            uint8_t *private_key,
                            const uint8_t *seed);

void ed25519_sign(uint8_t *signature,
                  const uint8_t *message, size_t message_len,
                  const uint8_t *public_key,
                  const uint8_t *private_key);
