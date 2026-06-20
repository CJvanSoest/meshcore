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

// Send an encrypted public-channel message (GRP_TXT, FLOOD).
bool send_chat_message(const char* text);
