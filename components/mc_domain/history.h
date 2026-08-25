// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Callback type passed to history_load_*; receives one decrypted record at a
// time. Called while the history mutex is held — keep the body short.
typedef void (*history_ring_add_fn)(const char* text, bool is_mine);

// Mount the µSD card (or fall back to the internal FAT store when no card is
// present), derive the per-device AES-128 key from prv_key, and prepare the
// on-disk layout. Idempotent; safe to call once at boot. locfs_init() must run
// first so the internal-store fallback is available.
void history_init(const uint8_t prv_key[32]);

// "off" | "ok" (SD) | "int" (internal FAT store) | "no-sd" | "err"
const char* history_status(void);
bool        history_is_ready(void);

// The chosen log root ("/sd/meshcore" or "/locfd/meshcore"), or NULL when
// history is disabled. Lets sibling stores (nodes.bin) share the same backend.
const char* history_root(void);

// True when the log is on the internal FAT store rather than an SD card. Callers
// that persist alongside history (nodes.bin) use this to apply size caps.
bool history_on_internal(void);

// Total capacity of the mounted µSD card in bytes, or 0 if none mounted.
// (Free space isn't exposed — statvfs is absent on the esp32p4 toolchain.)
uint64_t history_sd_capacity_bytes(void);

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

// ── Storage Viewer enumeration (issue #70) ──────────────────────────────────
// One on-disk conversation log. `id` is the 8-byte filename prefix (the channel
// secret prefix for channels, the peer pubkey prefix for DMs) — enough to map
// back to a channel/contact for a human label.
typedef struct {
    bool     is_dm;  // true = DM (dm/<pub-hex16>.bin), false = channel (ch/<secret-hex16>.bin)
    uint8_t  id[8];  // 8-byte filename prefix
    uint32_t bytes;  // file size on disk
} history_conv_t;

// Enumerate on-disk conversation logs (dm/ + ch/). Fills up to `max` entries,
// sorted by size descending, and returns the count. `out_total`, if non-NULL,
// receives the summed byte size of ALL logs (independent of `max`). Returns 0
// when SD is not mounted.
int history_list_conversations(history_conv_t* out, int max, uint64_t* out_total);

// Delete one conversation log by its 8-byte filename prefix + kind. Returns
// true if a file was removed.
bool history_delete_conversation(const uint8_t id[8], bool is_dm);

// Delete every conversation log (dm/ + ch/). Returns the number removed.
int history_clear_all(void);
