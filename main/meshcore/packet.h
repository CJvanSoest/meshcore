// SPDX-FileCopyrightText: 2025 Scott Powell / rippleradios.com
// SPDX-FileCopyrightText: 2025 Nicolai Electronics
// SPDX-License-Identifier: MIT

#pragma once

#include <stddef.h>
#include <stdint.h>

#define MESHCORE_MAX_HASH_SIZE        8
#define MESHCORE_PUB_KEY_SIZE         32
#define MESHCORE_PRV_KEY_SIZE         64
#define MESHCORE_SEED_SIZE            32
#define MESHCORE_SIGNATURE_SIZE       64
#define MESHCORE_MAX_ADVERT_DATA_SIZE 32
#define MESHCORE_CIPHER_KEY_SIZE      16
#define MESHCORE_CIPHER_BLOCK_SIZE    16
#define MESHCORE_CIPHER_MAC_SIZE      2
#define MESHCORE_PATH_HASH_SIZE       1
#define MESHCORE_MAX_PAYLOAD_SIZE     184
#define MESHCORE_MAX_PATH_SIZE        64
#define MESHCORE_MAX_TRANS_UNIT       255

typedef enum {
    MESHCORE_PAYLOAD_TYPE_REQ        = 0x0,
    MESHCORE_PAYLOAD_TYPE_RESPONSE   = 0x1,
    MESHCORE_PAYLOAD_TYPE_TXT_MSG    = 0x2,
    MESHCORE_PAYLOAD_TYPE_ACK        = 0x3,
    MESHCORE_PAYLOAD_TYPE_ADVERT     = 0x4,
    MESHCORE_PAYLOAD_TYPE_GRP_TXT    = 0x5,
    MESHCORE_PAYLOAD_TYPE_GRP_DATA   = 0x6,
    MESHCORE_PAYLOAD_TYPE_ANON_REQ   = 0x7,
    MESHCORE_PAYLOAD_TYPE_PATH       = 0x8,
    MESHCORE_PAYLOAD_TYPE_TRACE      = 0x9,
    MESHCORE_PAYLOAD_TYPE_MULTIPART  = 0xA,
    MESHCORE_PAYLOAD_TYPE_RAW_CUSTOM = 0xF,
} meshcore_payload_type_t;

typedef enum {
    MESHCORE_ROUTE_TYPE_TRANSPORT_FLOOD  = 0x0,
    MESHCORE_ROUTE_TYPE_FLOOD            = 0x1,
    MESHCORE_ROUTE_TYPE_DIRECT           = 0x2,
    MESHCORE_ROUTE_TYPE_TRANSPORT_DIRECT = 0x3,
} meshcore_route_type_t;

typedef struct {
    meshcore_payload_type_t type;
    meshcore_route_type_t   route;
    uint8_t                 version;
    uint16_t                transport_codes[2];
    uint8_t                 path_length;
    uint8_t                 path[MESHCORE_MAX_PATH_SIZE];
    uint8_t                 payload_length;
    uint8_t                 payload[MESHCORE_MAX_PAYLOAD_SIZE];
} meshcore_message_t;

int meshcore_serialize(const meshcore_message_t* message, uint8_t* out_data, uint8_t* out_size);
int meshcore_deserialize(uint8_t* data, uint8_t size, meshcore_message_t* out_message);
