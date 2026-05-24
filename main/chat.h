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
    uint32_t timestamp_ms;
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
