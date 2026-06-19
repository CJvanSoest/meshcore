// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
// SPDX-FileContributor: Ilias el Matani <hello@ilias.codes>
//
// The byte range an ADVERT's ed25519 signature covers. Upstream MeshCore
// signs pub_key + timestamp + everything past the signature field (flags,
// name, optional position) and skips the 64-byte signature slot itself.
// Getting this layout wrong yields a signature that is mathematically valid
// yet rejected by every verifier -- the exact failure mode of the original
// direct-advert bug. Kept pure (no signing, no crypto) so a host test can
// lock the offsets independently of the ed25519 implementation.

#pragma once

#include <stdint.h>

// Copy the signable slice of a serialized ADVERT `payload` into `out`
// (size >= MESHCORE_MAX_PAYLOAD_SIZE) and return its length. Returns 0 if
// the payload is too short to hold pub_key + timestamp.
uint8_t meshcore_advert_signable_bytes(const uint8_t *payload,
                                       uint8_t payload_len, uint8_t *out);
