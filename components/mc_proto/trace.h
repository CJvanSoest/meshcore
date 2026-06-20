// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// TRACE payload layout (MeshCore PAYLOAD_TYPE_TRACE = 0x09), the reachability
// probe used by the coverage test. Pure: lays out / reads the payload bytes
// (tag, auth, flags, hop hashes) so the byte format is host-tested independently
// of the radio, like advert_sign. The frame envelope (header + route + the
// accumulated per-hop SNR path) is built by meshcore_serialize in mc_rx; here we
// only own the payload body.
//
// upstream: meshcore-dev/MeshCore src/Mesh.cpp createTrace + onRecvPacket.
// Wire payload: tag[4 LE] | auth_code[4 LE] | flags[1] | hop_hashes[...]
// flags low 2 bits = path_sz (hop-hash size is 1 << path_sz bytes).

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MESHCORE_TRACE_HDR_LEN 9  // tag(4) + auth(4) + flags(1)

// Build a TRACE payload into out (caller-allocated, >= 9 + hashes_len). Returns
// the payload length, or 0 on a bad argument.
uint8_t meshcore_trace_build_payload(uint32_t tag, uint32_t auth_code, uint8_t path_sz, const uint8_t* hashes,
                                     uint8_t hashes_len, uint8_t* out);

// Parse a received TRACE payload. Fills the scalar fields and points
// *out_hashes at the hop-hash bytes inside payload (no copy). Returns false on a
// truncated payload.
bool meshcore_trace_parse(const uint8_t* payload, uint8_t payload_len, uint32_t* out_tag, uint32_t* out_auth,
                          uint8_t* out_flags, const uint8_t** out_hashes, uint8_t* out_hashes_len);

// Map our path-hash size in bytes (1/2/4) to the MeshCore path_sz (0/1/2).
uint8_t meshcore_trace_path_sz(uint8_t hash_bytes);
