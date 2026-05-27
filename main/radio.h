// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "lora.h"  // lora_protocol_lora_packet_t

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
// If ack_crc_out != NULL, returns the 4-byte CRC the receiver will echo back
// inside their PATH_RETURN — caller can store it on the chat_msg to track ACK.
bool send_dm_message(const char *text, const uint8_t *target_pub, uint8_t ack_crc_out[4]);

// Send an encrypted public-channel message (GRP_TXT, FLOOD).
bool send_chat_message(const char *text);
