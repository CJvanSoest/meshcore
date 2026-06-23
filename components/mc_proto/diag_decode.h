// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// Display-ready decode of a captured MeshCore frame for the Toolbox packet
// log. Pure: parses the literal on-air bytes (header + payload) into the
// fields the dissector view renders. No FreeRTOS, no domain, no UI, so the UI
// can call it without including the wire-protocol mirror (meshcore/*), and the
// logic is host-tested like advert_sign / region_limits.

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool     valid;          // header parsed OK
    uint8_t  ptype;          // meshcore_payload_type_t value (0x0..0xF)
    uint8_t  route;          // meshcore_route_type_t value (0..3)
    uint8_t  path_len;       // path byte count
    uint8_t  bytes_per_hop;  // 1, 2 or 3
    uint8_t  hops;           // path_len / bytes_per_hop (0 when bph == 0)
    uint16_t payload_len;

    // ADVERT only: the signer's identity + optional name / position.
    bool    has_pubkey;
    uint8_t pubkey[32];
    uint8_t role;  // meshcore_device_role_t value
    bool    has_name;
    char    name[33];
    bool    has_pos;
    int32_t lat_e6;
    int32_t lon_e6;

    // DM / PATH / ACK: the 1-byte destination + source key hashes on the wire.
    bool    has_hash;
    uint8_t dest_hash;
    uint8_t src_hash;
} diag_decoded_t;

// Parse `len` bytes of a captured frame. Returns true when at least the header
// parsed; per-type extraction is best-effort and skipped on truncated frames.
bool diag_decode(const uint8_t* frame, uint8_t len, diag_decoded_t* out);

// Short uppercase labels for the log columns. Always return a valid string.
const char* diag_type_name(uint8_t ptype);
const char* diag_route_name(uint8_t route);
const char* diag_role_name(uint8_t role);

// CSV header line (no trailing newline) for the packet-log SD export. Matches
// the column order produced by diag_csv_row().
#define DIAG_CSV_HEADER "ts_ms,dir,type,route,rssi_dbm,snr_db,len,raw_hex"

// Sentinel meaning "no signal metric" for rssi_dbm / snr_x4 (TX rows, or RX
// frames the radio reported no measurement for). Mirrors DIAG_RSSI_NONE in
// diag.h; duplicated here so this pure unit stays free of the mc_common ring.
#define DIAG_CSV_NO_SIGNAL 127

// Format one captured frame as a single CSV row (no trailing newline) into
// `out`, matching DIAG_CSV_HEADER. Pure: the UI packet-log SD export calls
// this per frame. Parameters are the raw diag_entry_t fields (passed by value
// so this stays free of the mc_common diag ring) plus the already-decoded
// view. `dir_is_tx` selects the "RX"/"TX" label and blanks the signal columns
// when set. rssi_dbm / snr_x4 equal to DIAG_CSV_NO_SIGNAL also blank their
// column. snr is emitted in dB (snr_x4 / 4, two decimals). raw[0..raw_len) is
// lower-case hex. Returns the byte count written (excl. NUL), or 0 if `cap` is
// too small to hold even the fixed columns.
int diag_csv_row(uint32_t ts_ms, bool dir_is_tx, int8_t rssi_dbm, int8_t snr_x4, uint8_t full_len, const uint8_t* raw,
                 uint8_t raw_len, const diag_decoded_t* d, char* out, int cap);
