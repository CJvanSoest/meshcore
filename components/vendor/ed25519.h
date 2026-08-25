/*
 * ed25519.h — Ed25519 signing + X25519 for ESP32-P4 / RISC-V 32-bit
 *
 * Backed by the public-domain ref10 implementation from orlp/ed25519 (Orson
 * Peters, zlib licence) vendored under ed25519/. Self-contained: its own
 * SHA-512 and field/group arithmetic, so it does NOT depend on the mbedtls
 * low-level MPI/SHA-512/ECP headers that mbedtls 4.1 (ESP-IDF 6) made private.
 * MIT licence for this wrapper header + the X25519 extras (ed25519_x25519_ext.c).
 */

#pragma once
#include <stddef.h>
#include <stdint.h>

/* Derive an Ed25519 key-pair from a 32-byte seed.
 *  public_key  [out] 32 bytes (compressed point A)
 *  private_key [out] 64 bytes: SHA-512(seed) with the scalar clamped */
void ed25519_create_keypair(uint8_t* public_key, uint8_t* private_key, const uint8_t* seed);

/* Sign a message. signature [out] 64 bytes (R || S). */
void ed25519_sign(uint8_t* signature, const uint8_t* message, size_t message_len, const uint8_t* public_key,
                  const uint8_t* private_key);

/* Verify a signature. Returns 1 on success, 0 on failure. */
int ed25519_verify(const uint8_t* signature, const uint8_t* message, size_t message_len, const uint8_t* public_key);

/* X25519 shared secret from our 64-byte Ed25519 private key and the peer's
 * 32-byte Ed25519 public key. Converts the Edwards public key to its
 * Montgomery u-coordinate, then runs the ladder (matches orlp key_exchange). */
void ed25519_key_exchange(uint8_t* shared_secret, const uint8_t* public_key, const uint8_t* private_key);

/* Variant that treats the 32-byte public_key bytes directly as the Montgomery
 * u-coordinate (no Edwards->Montgomery conversion). */
void ed25519_key_exchange_raw(uint8_t* shared_secret, const uint8_t* public_key, const uint8_t* private_key);

/* Convert an Ed25519 public key (compressed Edwards y) to its Curve25519
 * Montgomery u-coordinate: u = (1 + y) / (1 - y) mod (2^255 - 19). */
void ed25519_pub_to_x25519(uint8_t* curve25519_pub, const uint8_t* ed25519_pub);
