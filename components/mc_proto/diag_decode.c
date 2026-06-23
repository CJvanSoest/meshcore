// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "diag_decode.h"
#include <stdio.h>
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

int diag_csv_row(uint32_t ts_ms, bool dir_is_tx, int8_t rssi_dbm, int8_t snr_x4, uint8_t full_len, const uint8_t* raw,
                 uint8_t raw_len, const diag_decoded_t* d, char* out, int cap) {
    if (!out || cap <= 0) return 0;

    // Fixed columns first; raw_hex is appended byte-by-byte afterwards so a
    // tight `cap` truncates the hex tail rather than overrunning.
    char rssi[8];
    char snr[12];
    if (dir_is_tx || rssi_dbm == DIAG_CSV_NO_SIGNAL) {
        rssi[0] = '\0';
    } else {
        snprintf(rssi, sizeof(rssi), "%d", rssi_dbm);
    }
    if (dir_is_tx || snr_x4 == DIAG_CSV_NO_SIGNAL) {
        snr[0] = '\0';
    } else {
        // Quarter-dB fixed point → signed dB with two decimals, no float.
        int q    = snr_x4;  // promote past int8 before the sign-aware split
        int sign = q < 0 ? -1 : 1;
        int mag  = q * sign;
        snprintf(snr, sizeof(snr), "%s%d.%02d", sign < 0 ? "-" : "", mag / 4, (mag % 4) * 25);
    }

    const char* type  = (d && d->valid) ? diag_type_name(d->ptype) : "?";
    const char* route = (d && d->valid) ? diag_route_name(d->route) : "?";

    int n = snprintf(out, cap, "%lu,%s,%s,%s,%s,%s,%u,", (unsigned long)ts_ms, dir_is_tx ? "TX" : "RX", type, route,
                     rssi, snr, (unsigned)full_len);
    if (n < 0 || n >= cap) {
        out[cap - 1] = '\0';
        return 0;  // fixed columns alone overflowed; caller's buffer is too small
    }

    // Append raw[] as lower-case hex, stopping if the row buffer fills.
    static const char hexd[] = "0123456789abcdef";
    if (raw) {
        for (uint8_t i = 0; i < raw_len && n + 2 < cap; i++) {
            out[n++] = hexd[(raw[i] >> 4) & 0xF];
            out[n++] = hexd[raw[i] & 0xF];
        }
    }
    out[n] = '\0';
    return n;
}
