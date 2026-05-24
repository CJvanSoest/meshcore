// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "lora.h"  // lora_protocol_lora_packet_t

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
extern volatile int8_t   last_rx_rssi_dbm;          // 0 = no data
extern volatile int8_t   last_rx_snr_db_x4;         // SNR in 1/4 dB units
extern volatile int8_t   last_rx_signal_rssi_dbm;
extern volatile uint32_t last_rx_stats_ms;
extern volatile bool     last_rx_stats_valid;

extern volatile int8_t   noise_floor_dbm;           // 0 = no data
extern volatile bool     noise_floor_valid;
extern volatile bool     noise_floor_supported;     // false after NACK from old C6

// ── Last advert TX timestamp (ms since boot) — read by render_settings ───────
extern uint32_t last_advert_ms;

// ── Lifecycle ────────────────────────────────────────────────────────────────
// Create rx_mutex and start lora_rx_task, noise_floor_task, advert_task.
// Call after radio + identity + nodes/chat are initialised.
void radio_start_tasks(void);

// ── TX entry points (called from UI input) ───────────────────────────────────
// Broadcast a signed ADVERT (FLOOD) with our pub_key + role + name.
void send_advert(void);

// Send an encrypted DM (TXT_MSG) to a specific node by pub_key.
bool send_dm_message(const char *text, const uint8_t *target_pub);

// Send an encrypted public-channel message (GRP_TXT, FLOOD).
bool send_chat_message(const char *text);
