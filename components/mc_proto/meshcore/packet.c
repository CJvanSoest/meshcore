// SPDX-FileCopyrightText: 2025 Scott Powell / rippleradios.com
// SPDX-FileCopyrightText: 2025 Nicolai Electronics
// SPDX-License-Identifier: MIT

#include "packet.h"
#include <stdint.h>
#include <string.h>

#define member_size(type, member) (sizeof(((type *)0)->member))

#define PACKET_HEADER_ROUTE_SHIFT 0
#define PACKET_HEADER_ROUTE_MASK  0x03
#define PACKET_HEADER_TYPE_SHIFT  2
#define PACKET_HEADER_TYPE_MASK   0x0F
#define PACKET_HEADER_VER_SHIFT   6
#define PACKET_HEADER_VER_MASK    0x03

typedef struct __attribute__((packed)) {
    uint8_t  header;
    uint16_t transport_codes[0];
} meshcore_line_header_t;

int meshcore_serialize(const meshcore_message_t* message, uint8_t* out_data, uint8_t* out_size) {
    if (out_data == NULL) return -1;
    memset(out_data, 0, MESHCORE_MAX_TRANS_UNIT);
    if (message->path_length > MESHCORE_MAX_PATH_SIZE || message->payload_length > MESHCORE_MAX_PAYLOAD_SIZE) return -1;

    uint8_t position = 0;
    meshcore_line_header_t* line_header = (meshcore_line_header_t*)&out_data[position];
    position += sizeof(meshcore_line_header_t);

    line_header->header  = (message->route   & PACKET_HEADER_ROUTE_MASK) << PACKET_HEADER_ROUTE_SHIFT;
    line_header->header += (message->type    & PACKET_HEADER_TYPE_MASK)  << PACKET_HEADER_TYPE_SHIFT;
    line_header->header += (message->version & PACKET_HEADER_VER_MASK)   << PACKET_HEADER_VER_SHIFT;

    if (message->route == MESHCORE_ROUTE_TYPE_TRANSPORT_FLOOD ||
        message->route == MESHCORE_ROUTE_TYPE_TRANSPORT_DIRECT) {
        // Two transport_codes (uint16 each) = 4 bytes on the wire.
        memcpy(&out_data[position], message->transport_codes, sizeof(message->transport_codes));
        position += sizeof(message->transport_codes);
    }

    {
        uint8_t bph = message->bytes_per_hop ? message->bytes_per_hop : 1;
        if (bph > 3) return -1;
        if (message->path_length % bph != 0) return -1;
        uint8_t hop_count = message->path_length / bph;
        if (hop_count > 0x3F) return -1;
        out_data[position] = ((uint8_t)((bph - 1) & 0x03) << 6) | (hop_count & 0x3F);
    }
    position           += sizeof(uint8_t);
    memcpy(&out_data[position], message->path, message->path_length);
    position += message->path_length;
    memcpy(&out_data[position], message->payload, message->payload_length);
    position += message->payload_length;

    *out_size = position;
    return 0;
}

int meshcore_deserialize(uint8_t* data, uint8_t size, meshcore_message_t* out_message) {
    if (out_message == NULL || data == NULL) return -1;
    memset(out_message, 0, sizeof(meshcore_message_t));

    uint8_t position = 0;
    if (size < sizeof(meshcore_line_header_t) || size > MESHCORE_MAX_TRANS_UNIT) return -1;

    meshcore_line_header_t* line_header = (meshcore_line_header_t*)&data[position];
    position += sizeof(meshcore_line_header_t);

    out_message->route   = (line_header->header >> PACKET_HEADER_ROUTE_SHIFT) & PACKET_HEADER_ROUTE_MASK;
    out_message->type    = (line_header->header >> PACKET_HEADER_TYPE_SHIFT)  & PACKET_HEADER_TYPE_MASK;
    out_message->version = (line_header->header >> PACKET_HEADER_VER_SHIFT)   & PACKET_HEADER_VER_MASK;

    if (out_message->route == MESHCORE_ROUTE_TYPE_TRANSPORT_FLOOD ||
        out_message->route == MESHCORE_ROUTE_TYPE_TRANSPORT_DIRECT) {
        // Two transport_codes (uint16 each) = 4 bytes on the wire.
        if (size - position < sizeof(out_message->transport_codes)) return -1;
        memcpy(out_message->transport_codes, &data[position], sizeof(out_message->transport_codes));
        position += sizeof(out_message->transport_codes);
    }

    if (size - position < sizeof(uint8_t)) return -1;
    {
        uint8_t plb       = data[position];
        uint8_t hop_count = plb & 0x3F;
        uint8_t bph       = ((plb >> 6) & 0x03) + 1;  // 00->1, 01->2, 10->3
        if (bph > 3) return -1;
        out_message->bytes_per_hop = bph;
        out_message->path_length   = hop_count * bph;
    }
    position += sizeof(uint8_t);

    if (out_message->path_length > MESHCORE_MAX_PATH_SIZE) return -1;
    if (size - position < out_message->path_length) return -1;
    memcpy(out_message->path, &data[position], out_message->path_length);
    position += out_message->path_length;

    out_message->payload_length = size - position;
    if (out_message->payload_length > MESHCORE_MAX_PAYLOAD_SIZE) return -1;
    memcpy(out_message->payload, &data[position], out_message->payload_length);

    return 0;
}
