// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "trace.h"
#include <string.h>

uint8_t meshcore_trace_build_payload(uint32_t tag, uint32_t auth_code, uint8_t path_sz, const uint8_t* hashes,
                                     uint8_t hashes_len, uint8_t* out) {
    if (out == NULL) return 0;
    if (hashes_len > 0 && hashes == NULL) return 0;

    uint8_t i = 0;
    memcpy(&out[i], &tag, 4);
    i += 4;
    memcpy(&out[i], &auth_code, 4);
    i        += 4;
    out[i++]  = (uint8_t)(path_sz & 0x03);
    if (hashes_len > 0) {
        memcpy(&out[i], hashes, hashes_len);
        i += hashes_len;
    }
    return i;
}

bool meshcore_trace_parse(const uint8_t* payload, uint8_t payload_len, uint32_t* out_tag, uint32_t* out_auth,
                          uint8_t* out_flags, const uint8_t** out_hashes, uint8_t* out_hashes_len) {
    if (payload == NULL || payload_len < MESHCORE_TRACE_HDR_LEN) return false;

    if (out_tag) memcpy(out_tag, &payload[0], 4);
    if (out_auth) memcpy(out_auth, &payload[4], 4);
    if (out_flags) *out_flags = payload[8];
    if (out_hashes) *out_hashes = &payload[MESHCORE_TRACE_HDR_LEN];
    if (out_hashes_len) *out_hashes_len = (uint8_t)(payload_len - MESHCORE_TRACE_HDR_LEN);
    return true;
}

uint8_t meshcore_trace_path_sz(uint8_t hash_bytes) {
    if (hash_bytes >= 4) return 2;  // 4-byte hash
    if (hash_bytes >= 2) return 1;  // 2-byte hash
    return 0;                       // 1-byte hash
}
