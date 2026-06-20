// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "diag_decode.h"
#include <string.h>
#include "meshcore/packet.h"
#include "meshcore/payload/advert.h"

bool diag_decode(const uint8_t* frame, uint8_t len, diag_decoded_t* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!frame || len == 0) return false;

    // meshcore_deserialize takes a mutable buffer; copy so the caller's capture
    // stays read-only (and so a const frame argument is accepted).
    uint8_t buf[MESHCORE_MAX_TRANS_UNIT];
    uint8_t n = len > (uint8_t)sizeof(buf) ? (uint8_t)sizeof(buf) : len;
    memcpy(buf, frame, n);

    meshcore_message_t msg;
    if (meshcore_deserialize(buf, n, &msg) < 0) return false;

    out->valid         = true;
    out->ptype         = (uint8_t)msg.type;
    out->route         = (uint8_t)msg.route;
    out->path_len      = msg.path_length;
    out->bytes_per_hop = msg.bytes_per_hop;
    out->hops          = msg.bytes_per_hop ? (msg.path_length / msg.bytes_per_hop) : 0;
    out->payload_len   = msg.payload_length;

    switch (msg.type) {
        case MESHCORE_PAYLOAD_TYPE_ADVERT: {
            meshcore_advert_t a;
            if (meshcore_advert_deserialize(msg.payload, msg.payload_length, &a) >= 0) {
                out->has_pubkey = true;
                memcpy(out->pubkey, a.pub_key, sizeof(out->pubkey));
                out->role = (uint8_t)a.role;
                if (a.name_valid) {
                    out->has_name = true;
                    strncpy(out->name, a.name, sizeof(out->name) - 1);
                }
                if (a.position_valid) {
                    out->has_pos = true;
                    out->lat_e6  = a.position_lat;
                    out->lon_e6  = a.position_lon;
                }
            }
            break;
        }
        case MESHCORE_PAYLOAD_TYPE_TXT_MSG:
        case MESHCORE_PAYLOAD_TYPE_PATH:
        case MESHCORE_PAYLOAD_TYPE_ACK:
            // dest_hash, src_hash are the first two payload bytes for the
            // direct-addressed types (1-byte truncated key hashes on the wire).
            if (msg.payload_length >= 2) {
                out->has_hash  = true;
                out->dest_hash = msg.payload[0];
                out->src_hash  = msg.payload[1];
            }
            break;
        default:
            break;
    }
    return true;
}

const char* diag_type_name(uint8_t ptype) {
    switch (ptype) {
        case MESHCORE_PAYLOAD_TYPE_REQ:
            return "REQ";
        case MESHCORE_PAYLOAD_TYPE_RESPONSE:
            return "RESP";
        case MESHCORE_PAYLOAD_TYPE_TXT_MSG:
            return "DM";
        case MESHCORE_PAYLOAD_TYPE_ACK:
            return "ACK";
        case MESHCORE_PAYLOAD_TYPE_ADVERT:
            return "ADVERT";
        case MESHCORE_PAYLOAD_TYPE_GRP_TXT:
            return "CHAN";
        case MESHCORE_PAYLOAD_TYPE_GRP_DATA:
            return "GRPDAT";
        case MESHCORE_PAYLOAD_TYPE_ANON_REQ:
            return "ANONRQ";
        case MESHCORE_PAYLOAD_TYPE_PATH:
            return "PATH";
        case MESHCORE_PAYLOAD_TYPE_TRACE:
            return "TRACE";
        case MESHCORE_PAYLOAD_TYPE_MULTIPART:
            return "MULTI";
        case MESHCORE_PAYLOAD_TYPE_RAW_CUSTOM:
            return "RAW";
        default:
            return "?";
    }
}

const char* diag_route_name(uint8_t route) {
    switch (route) {
        case MESHCORE_ROUTE_TYPE_TRANSPORT_FLOOD:
            return "T-FLOOD";
        case MESHCORE_ROUTE_TYPE_FLOOD:
            return "FLOOD";
        case MESHCORE_ROUTE_TYPE_DIRECT:
            return "DIRECT";
        case MESHCORE_ROUTE_TYPE_TRANSPORT_DIRECT:
            return "T-DIRECT";
        default:
            return "?";
    }
}

const char* diag_role_name(uint8_t role) {
    switch (role) {
        case MESHCORE_DEVICE_ROLE_CHAT_NODE:
            return "Chat";
        case MESHCORE_DEVICE_ROLE_REPEATER:
            return "Repeater";
        case MESHCORE_DEVICE_ROLE_ROOM_SERVER:
            return "Room";
        case MESHCORE_DEVICE_ROLE_SENSOR:
            return "Sensor";
        default:
            return "Unknown";
    }
}
