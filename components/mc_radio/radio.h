// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
// SPDX-FileContributor: Ilias el Matani <hello@ilias.codes>

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lora.h"             // lora_protocol_lora_packet_t
#include "meshcore/packet.h"  // meshcore_message_t for the RX sink + TX primitive

// Shared LoRa radio handle (radio v3.0.0 handle-based API).
// Defined in radio.c; used by main.c and settings_nvs.c too.
extern lora_handle_t lora_handle;

// SX126x silicon version string reported by C6 via lora_get_status()
// (e.g. "sx1262 V20 2002"). Populated once after lora_init_remote() in main.c
// boot path; "?" until then.
#define RADIO_FW_VERSION_LEN 17
extern char radio_fw_version[RADIO_FW_VERSION_LEN];

// tanmatsu-radio app firmware version (esp_app_desc->version), queried via
// GET_FW_VERSION. Empty string if C6 firmware lacks the cmd — in that case
// render.c falls back to TANMATSU_RADIO_FW_LABEL.
#define RADIO_FW_APP_VERSION_LEN 33
extern char radio_fw_app_version[RADIO_FW_APP_VERSION_LEN];

// ── RX ring buffer (raw packets, for debug display) ──────────────────────────
#define RX_BUF_SIZE 32

typedef struct {
    lora_protocol_lora_packet_t pkt;
    uint32_t                    timestamp_ms;
} rx_entry_t;

extern rx_entry_t        rx_buf[RX_BUF_SIZE];
extern int               rx_head;
extern int               rx_count;
extern SemaphoreHandle_t rx_mutex;

// True once lora_rx_task has successfully entered RX mode at least once.
extern bool lora_rx_ok;

// ── RF statistics (last RX packet + ambient noise floor) ─────────────────────
extern volatile int8_t   last_rx_rssi_dbm;   // 0 = no data
extern volatile int8_t   last_rx_snr_db_x4;  // SNR in 1/4 dB units
extern volatile int8_t   last_rx_signal_rssi_dbm;
extern volatile uint32_t last_rx_stats_ms;
extern volatile bool     last_rx_stats_valid;

extern volatile int8_t noise_floor_dbm;  // 0 = no data
extern volatile bool   noise_floor_valid;
extern volatile bool   noise_floor_supported;  // false after NACK from old C6

// ── Last advert TX timestamp (ms since boot) — read by render_settings ───────
extern uint32_t last_advert_ms;

// ── Duty-cycle accounting (rolling 1h, ms-on-air) ────────────────────────────
// Updated by radio.c TX paths; read-only for render. dc_budget_ms == 0 means
// either no country/sub-band match or unlimited DC (US/AU/etc.); render should
// show "—" in that case. dc_last_tx_blocked is sticky until cleared by UI.
extern volatile uint32_t dc_used_ms;
extern volatile uint32_t dc_budget_ms;
extern volatile bool     dc_last_tx_blocked;

// ── Lifecycle ────────────────────────────────────────────────────────────────
// Create rx_mutex and start lora_rx_task, noise_floor_task, advert_task.
// Call after radio + identity + nodes/chat are initialised.
void radio_start_tasks(void);

// ── RX sink (radio decodes + dedups; the app layer handles delivery) ─────────
// lora_rx_task deserializes each packet, flags flood duplicates, and hands the
// raw typed message to the registered sink. The sink (mc_rx) owns decrypt,
// domain writes and notifications, so the radio layer stays domain-free on RX.
// Duplicates are passed through (not dropped at the radio layer) so the domain
// can use the echo of our own channel flood as a relay confirmation; the sink
// drops every other duplicate.
typedef struct {
    int8_t              rssi_dbm;
    int8_t              snr_db_x4;
    int8_t              signal_rssi_dbm;
    uint32_t            now_ms;
    lora_packet_stats_t stats;         // raw C6 stats (the advert handler stores them)
    bool                is_duplicate;  // flood retransmit: sink drops it, except the
                                       // echo of our own channel message (relay confirm)
} radio_rx_meta_t;

typedef void (*radio_rx_sink_fn)(const meshcore_message_t* msg, const radio_rx_meta_t* meta);
void radio_set_rx_sink(radio_rx_sink_fn sink);

// Finalize + transmit a composed message: apply region scope, serialize, gate
// on the duty-cycle budget, then send over the C6. Returns false on a
// serialize failure or an exhausted budget. Used by the TX paths and by the
// RX ACK return in mc_rx.
bool radio_tx_message(meshcore_message_t* msg);

// Reconcile the LoRa config in NVS with the live C6 radio over lora_handle.
// load_lora_config pulls from NVS then prefers/pushes against the C6; save
// writes NVS and pushes to the C6 (re-entering RX). Moved here from
// settings_nvs so the L1 config store has no radio dependency.
void load_lora_config(void);
void save_lora_config(void);
