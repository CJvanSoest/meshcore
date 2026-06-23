// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
// SPDX-FileContributor: Ilias el Matani <hello@ilias.codes>
//
// MeshCore application layer: the RX handlers (behind the radio sink) and the
// TX composers (called by the UI). Keeps radio.c as pure transport.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Register the RX sink with the radio layer. Call once at boot, before
// radio_start_tasks, so received packets are decrypted and delivered.
void mc_rx_init(void);

// Start the periodic ADVERT TX task. Call after radio_start_tasks.
void mc_rx_start_advert_task(void);

// ── TX entry points (called from the UI) ─────────────────────────────────────
// Broadcast a signed ADVERT (FLOOD) with our pub_key + role + name.
void send_advert(void);
// Same advert, route=DIRECT — LoRa neighbours only, low-traffic alternative.
void send_advert_direct(void);

// Send an encrypted DM (TXT_MSG) to a node by pub_key. If ack_crc_out != NULL,
// returns the 4-byte CRC the receiver echoes back in its PATH_RETURN so the
// caller can track the ACK.
bool send_dm_message(const char* text, const uint8_t* target_pub, uint8_t ack_crc_out[4]);

// Send an encrypted channel message (GRP_TXT, FLOOD) on the active channel.
// When out_fp is non-NULL and the send succeeds, it receives the first 4 bytes
// of the GRP_TXT payload — the fingerprint ch_arm_relay/ch_mark_relayed_by_fp
// use to confirm the flood was relayed when it echoes back.
bool send_chat_message(const char* text, uint8_t out_fp[4]);

// Send a MeshCore TRACE (reachability probe) to target_pub's hash with the given
// 32-bit tag, DIRECT-routed with the repeater as its one-hop path. The returning
// frame is matched by tag in rx_handle_trace. Used by the coverage test.
bool send_trace(const uint8_t* target_pub, uint32_t tag);

// Coverage test: ping `pub` COVERAGE_PINGS times (10 s apart) on a background
// task, recording reachability + a GPS-stamped CSV row per attempt. No-op if a
// run is already in progress (coverage_busy()). `name`/GPS are used only for
// the log row. Defined in mc_rx; results live in mc_domain/coverage.
void coverage_ping_start(const uint8_t* pub, const char* name, int32_t lat_e6, int32_t lon_e6, bool gps_valid);
