// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Callback type passed to history_load_*; receives one decrypted record at a
// time. Called while the history mutex is held — keep the body short.
typedef void (*history_ring_add_fn)(const char* text, bool is_mine);

// Mount the µSD card, derive the per-device AES-128 key from prv_key, and
// prepare the on-disk layout. Idempotent; safe to call once at boot.
void history_init(const uint8_t prv_key[32]);

// "off" | "ok" | "no-sd" | "err"
const char* history_status(void);
bool        history_is_ready(void);

// Per-channel history keyed by the 16-byte channel secret (file name uses an
// 8-byte secret prefix as hex, mirroring the per-DM scheme). Each channel keeps
// its own log so messages don't bleed across channels.
void history_append_channel(const uint8_t secret[16], const char* text, bool is_mine);
void history_load_channel(const uint8_t secret[16], history_ring_add_fn add);
void history_delete_channel(const uint8_t secret[16]);

void history_append_dm(const uint8_t peer_pub[32], const char* text, bool is_mine);
void history_load_dm(const uint8_t peer_pub[32], history_ring_add_fn add);

// Remove on-disk DM history file. No-op if SD is not mounted.
void history_delete_dm(const uint8_t peer_pub[32]);
