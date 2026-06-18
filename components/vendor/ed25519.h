/*
 * ed25519.h — Ed25519 signing for ESP32-P4 / RISC-V 32-bit
 *
 * Based on public domain NaCl/SUPERCOP ref10 by Daniel J. Bernstein et al.
 * Adapted to use mbedTLS SHA-512 and pure C (no inline assembly).
 * MIT licence for the wrapper / adaptation layer.
 */

#pragma once
#include <stdint.h>
#include <stddef.h>

/*
 * ed25519_create_keypair
 *
 * Derive an Ed25519 key-pair from a 32-byte random seed.
 *
 *  seed        [in]  32 bytes of random input
 *  public_key  [out] 32 bytes — the Ed25519 public key (compressed point A)
 *  private_key [out] 64 bytes — SHA-512(seed), with h[0] clamped;
 *                    bytes [0..31] are the scalar a,
 *                    bytes [32..63] are the nonce prefix used in signing.
 */
void ed25519_create_keypair(uint8_t *public_key,
                            uint8_t *private_key,
                            const uint8_t *seed);

/*
 * ed25519_sign
 *
 * Sign a message with an Ed25519 key-pair.
 *
 *  signature   [out] 64 bytes — the Ed25519 signature (R || S)
 *  message     [in]  message bytes
 *  message_len [in]  length of message in bytes
 *  public_key  [in]  32 bytes — the public key returned by create_keypair
 *  private_key [in]  64 bytes — the private key returned by create_keypair
 */
void ed25519_sign(uint8_t *signature,
                  const uint8_t *message, size_t message_len,
                  const uint8_t *public_key,
                  const uint8_t *private_key);

/* X25519 key exchange using Ed25519 keypair.
 * Derives a 32-byte shared secret from our private key and the peer's public key.
 * private_key: 64-byte expanded Ed25519 private key (first 32 bytes = scalar)
 * public_key:  32-byte Ed25519 public key of the peer */
void ed25519_key_exchange(uint8_t *shared_secret,
                          const uint8_t *public_key,
                          const uint8_t *private_key);

/* Variant that skips Edwards→Montgomery conversion (raw bytes as u-coordinate) */
void ed25519_key_exchange_raw(uint8_t *shared_secret,
                               const uint8_t *public_key,
                               const uint8_t *private_key);

/* Convert an Ed25519 public key (Edwards y-coordinate, compressed 32 bytes)
 * to its Curve25519 Montgomery u-coordinate (32 bytes, little-endian).
 * u = (1 + y) / (1 - y) mod (2^255 - 19) */
void ed25519_pub_to_x25519(uint8_t *curve25519_pub, const uint8_t *ed25519_pub);
