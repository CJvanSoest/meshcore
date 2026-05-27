// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "app_config.h"
#include "meshcore/packet.h"
#include "meshcore/payload/advert.h"  // MESHCORE_MAX_NAME_SIZE

#define MAX_INPUT_LEN 120

typedef struct {
    bool     active;
    bool     is_mine;
    char     text[MAX_MSG_TEXT];
    uint32_t timestamp_ms;     // device-tick — for relative timing, lost on reboot
    uint32_t timestamp_unix;   // epoch seconds; 0 = unknown (history reload / no SNTP)
    uint8_t  hops;             // 0 = own / direct; 0xFF = unknown (e.g. SD reload)
    uint8_t  ack_state;        // 0 = N/A (theirs / channel), 1 = waiting, 2 = acked
    uint8_t  ack_crc[4];       // for own DMs: matches incoming PATH_RETURN ACK
} chat_msg_t;

// ── DM ring (per-peer; cleared + reloaded by dm_select_target) ───────────────
extern chat_msg_t        chat_msgs[MAX_CHAT_MSGS];
extern int               chat_head;
extern int               chat_count;
extern int               chat_scroll;
extern SemaphoreHandle_t chat_mutex;

// ── Channel ring (GRP_TXT public channel) ────────────────────────────────────
extern chat_msg_t        ch_msgs[MAX_CHAT_MSGS];
extern int               ch_head;
extern int               ch_count;
extern int               ch_scroll;
extern SemaphoreHandle_t ch_mutex;

// ── Input text editor state ──────────────────────────────────────────────────
extern char chat_input[MAX_INPUT_LEN + 1];
extern int  chat_input_len;
extern bool chat_typing;

// ── Emoji picker overlay state (active during chat_typing) ───────────────────
extern bool emoji_picker_active;
extern int  emoji_picker_cursor;  // index into EMOJI_SET (0..EMOJI_COUNT-1)

// ── DM tab inbox-view state ──────────────────────────────────────────────────
extern bool dm_inbox_mode;
extern int  dm_inbox_cursor;
extern int  dm_inbox_scroll;

// ── DM target (currently selected peer for the DM tab) ───────────────────────
extern bool    dm_target_set;
extern uint8_t dm_target_pub[MESHCORE_PUB_KEY_SIZE];
extern char    dm_target_name[MESHCORE_MAX_NAME_SIZE + 1];

// ── Unread notification LED state ────────────────────────────────────────────
extern bool led_dm_pending;
extern bool led_channel_pending;

// ── Per-tab unread counters (rendered as badges on the tab bar) ──────────────
extern int dm_unread_count;
extern int channel_unread_count;

// ── Public channel ───────────────────────────────────────────────────────────
extern const uint8_t PUBLIC_CHANNEL_KEY[16];
extern uint8_t       channel_hash;  // first byte of SHA256(PUBLIC_CHANNEL_KEY)

// ── Lifecycle ────────────────────────────────────────────────────────────────
// Creates chat_mutex + ch_mutex and computes channel_hash. Call once at boot.
void chat_init(void);

// ── DM messages ──────────────────────────────────────────────────────────────
// Push a system/status string into the active DM ring (RAM only, not persisted).
void chat_add_message(const char *text, bool is_mine);

// Persist a DM to the peer's on-disk history; if that peer is the active DM
// target, also push it onto the visible ring.
void chat_add_dm(const char *text, bool is_mine, const uint8_t peer_pub[32]);

// Per-message metadata helpers (call right after add; the most-recent ring
// entry — the one just appended — gets the fields). Safe to call from RX path.
void chat_set_meta_dm(uint8_t hops);
void chat_set_meta_channel(uint8_t hops);
void chat_arm_ack_dm(const uint8_t ack_crc[4]);

// Look up an own outgoing DM by ACK CRC and mark it acked. Returns true on hit.
bool chat_mark_ack_by_crc(const uint8_t ack_crc[4]);

// Switch the active DM peer, clear the visible ring, and reload from SD.
void dm_select_target(const uint8_t pub[32], const char *name);

// ── Channel messages ─────────────────────────────────────────────────────────
// Add a channel message to the ring AND append to the channel history file.
void ch_add_message(const char *text, bool is_mine);

// ── Misc ─────────────────────────────────────────────────────────────────────
// Sync the coprocessor notification LED to the current pending-flag state
// (green = DM, blue = channel, off = none).
void update_notification_led(void);

// Callbacks for history_load_* (declared so main can pass them as args).
void chat_ring_add_from_disk(const char *text, bool is_mine);
void ch_ring_add_from_disk(const char *text, bool is_mine);
