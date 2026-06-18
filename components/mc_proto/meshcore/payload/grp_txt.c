// SPDX-FileCopyrightText: 2025 Scott Powell / rippleradios.com
// SPDX-FileCopyrightText: 2025 Nicolai Electronics
// SPDX-License-Identifier: MIT

#include "grp_txt.h"
#include <string.h>
#include "../packet.h"

int meshcore_grp_txt_deserialize(uint8_t* payload, uint8_t size, meshcore_grp_txt_t* out_grp_text) {
    if (!out_grp_text || !payload) return -1;
    if (size < 1 + MESHCORE_CIPHER_MAC_SIZE) return -1;

    memset(out_grp_text, 0, sizeof(meshcore_grp_txt_t));

    uint8_t pos = 0;
    out_grp_text->channel_hash = payload[pos++];
    memcpy(out_grp_text->mac, &payload[pos], MESHCORE_CIPHER_MAC_SIZE);
    pos += MESHCORE_CIPHER_MAC_SIZE;
    out_grp_text->data_length = size - pos;
    if (out_grp_text->data_length > sizeof(out_grp_text->data)) return -1;
    memcpy(out_grp_text->data, &payload[pos], out_grp_text->data_length);
    return 0;
}

int meshcore_grp_txt_serialize(const meshcore_grp_txt_t* grp_text, uint8_t* out_payload, uint8_t* out_size) {
    if (!grp_text || !out_payload) return -1;

    memset(out_payload, 0, MESHCORE_MAX_PAYLOAD_SIZE);
    uint8_t pos = 0;
    out_payload[pos++] = grp_text->channel_hash;
    memcpy(&out_payload[pos], grp_text->mac, MESHCORE_CIPHER_MAC_SIZE);
    pos += MESHCORE_CIPHER_MAC_SIZE;
    memcpy(&out_payload[pos], grp_text->data, grp_text->data_length);
    pos += grp_text->data_length;
    *out_size = pos;
    return 0;
}
