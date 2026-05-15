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
