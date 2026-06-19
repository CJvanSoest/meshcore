// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

// Persistent node identity: an Ed25519 keypair derived from a per-device seed
// in NVS. Pub/priv key bytes are exposed read-only via externs so the rest of
// the app can use them directly without copying.
extern uint8_t node_pub_key[32];
extern uint8_t node_prv_key[64];

bool identity_is_ready(void);
bool identity_sntp_synced(void);

// Generate-or-restore the keypair. Run once at boot before any code that
// signs or decrypts messages.
void identity_init(void);

// SNTP sync callback — call from your esp_sntp_set_time_sync_notification_cb
// hook to mark identity_sntp_synced() = true and persist the last-known time
// to NVS for offline-boot restore.
struct timeval;
void identity_sntp_sync_cb(struct timeval *tv);

// Mark the system clock as authoritative and persist the current epoch to
// NVS as last-known-good. Call after a successful bsp_rtc_update_time()
// (i.e. when the launcher/firmware already SNTP-synced) so downstream code
// that gates on identity_sntp_synced() (e.g. NVS-time fallback) sees the
// clock as trusted.
void identity_mark_time_synced(void);

// RFC 8032 Ed25519 self-test results. Set by identity_init() at boot.
// True iff TV1 keypair derivation + deterministic sign produced the expected
// reference output. False here means every outgoing advert/DM signature will
// be rejected by RFC8032 verifiers and the mesh will silently drop us.
extern bool ed25519_tv1_keypair_ok;
extern bool ed25519_tv1_sign_ok;
