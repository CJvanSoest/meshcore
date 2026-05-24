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
