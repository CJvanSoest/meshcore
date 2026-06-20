// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// Companion-protocol receiver running on top of the default USB-Serial-JTAG
// console. Reads framed commands from the host (start byte '<' + uint16 LE
// length + payload, per [[mc-companion-protocol]]) and dispatches them; the
// only opcode wired today is COMPANION_CMD_SET_ADVERT_LATLON, which writes
// the pushed coords into the same NVS keys that the PA1010D scan and manual
// entry already use.
//
// Coexists with ESP_LOG output on the same JTAG channel: ESP_LOG bytes don't
// start with '<', so the parser silently discards them while waiting for a
// frame. The host-side script just needs to ignore log lines when reading
// back.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "settings_nvs.h"  // for gps_source_t

// Start the FreeRTOS reader task. Safe to call once at boot, after
// load_gps_coords() so the source enum is populated. Returns true on success.
bool companion_transport_init(void);

// Transport hook: companion handlers call this through the dispatcher to push
// a reply back to the host. BLE's hook notifies on the TX characteristic;
// USB-CDC passes NULL because mixing structured replies into the ESP_LOG
// stream would be unparseable on the host side.
typedef void (*companion_response_sender_t)(const uint8_t* frame, size_t len);

// Feed *framed* bytes into the protocol parser. Used by the USB-CDC reader
// task on the serial stream where there are no inherent message boundaries,
// so the upstream parser scans for '<' + uint16 LE len + payload.
//
// Single global parser + sender state means concurrent feeds from multiple
// transports would interleave; the implementation serialises with a mutex.
void companion_dispatch_frame(const uint8_t* buf, size_t len, gps_source_t src, companion_response_sender_t sender);

// Feed *one already-unframed command* (opcode byte + args) straight into
// the dispatcher. Used by BLE where each ATT write boundary IS the message
// boundary -- upstream MeshCore peers (iPhone app, T-Beam companion) send
// the raw command, not the serial-line wrapped form.
void companion_dispatch_raw(const uint8_t* cmd, size_t len, gps_source_t src, companion_response_sender_t sender);

// Called from inside opcode handlers to push a reply. Builds [response_code |
// args] (no '<' / '>' framing) and routes it through the sender registered
// for the current dispatch. Sender is responsible for any transport-level
// framing (BLE notify needs none -- one notify = one packet). No-op when
// sender is NULL. args may be NULL when args_len == 0.
void companion_send_response(uint8_t response_code, const void* args, size_t args_len);
