// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CHANNELS_MAX           8
#define CHANNEL_NAME_MAX_LEN   23   // 24 bytes incl. null; covers "#abcdefghijklmnopqrstuv"
#define CHANNEL_SECRET_LEN     16   // MeshCore CIPHER_KEY_SIZE

typedef struct {
    bool    active;                              // slot in use
    char    name[CHANNEL_NAME_MAX_LEN + 1];      // human label incl. '#' for non-Public
    uint8_t secret[CHANNEL_SECRET_LEN];          // 16-byte AES/HMAC key (PSK or SHA256(name)[0..15])
    uint8_t hash;                                // SHA256(secret, 16)[0] — channel_hash on the wire
} channel_t;

// In-memory channel table. channels[0] is always the public "Public" channel
// using the upstream MeshCore PUBLIC_GROUP_PSK; cannot be removed.
extern channel_t channels[CHANNELS_MAX];
extern int       channel_count;     // number of active slots in [0..channel_count)
extern int       active_channel_idx; // selected slot for TX

// Called once at boot, after chat_init. Bootstraps channels[0]=Public and
// loads any user-added channels from NVS blob "mc.channels".
void channels_init(void);

// Derive a 16-byte secret from a channel name following Renze's spec:
// secret = SHA256(name including '#')[0..15]. Used for non-Public channels.
void channels_derive_secret_from_name(const char *name, uint8_t out_secret[CHANNEL_SECRET_LEN]);

// Add a channel by name (e.g. "#nl"). Derives the secret from the name,
// computes the hash, persists to NVS. Returns slot idx on success or -1 if
// the table is full or a slot with the same name/secret already exists.
int channels_add_by_name(const char *name);

// Add a channel with an explicit 16-byte secret + optional display name.
// Used by future QR / share-link import paths. Returns slot idx or -1.
int channels_add_with_secret(const char *name, const uint8_t secret[CHANNEL_SECRET_LEN]);

// Remove the channel at idx. idx=0 (Public) cannot be removed; returns false.
bool channels_remove(int idx);

// Find the slot matching the given on-the-wire channel_hash byte. Returns
// the slot idx or -1 if no match. Note: a hash match is only a hint;
// callers should still verify the MAC against the channel's secret.
int channels_find_by_hash(uint8_t hash);

// Persist channels[1..channel_count-1] (skip hardcoded Public) to NVS blob
// "mc.channels". Called automatically by channels_add_*/channels_remove.
void channels_save_nvs(void);

// ── Channel-tab UI state ─────────────────────────────────────────────────────
// channel_list_mode  — true: show channel picker; false: show chat for active
// channel_list_cursor — selection in the picker
// channel_adding     — text-input flow active (sharing field_edit_buf)
extern bool channel_list_mode;
extern int  channel_list_cursor;
extern bool channel_adding;
