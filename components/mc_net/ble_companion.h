// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// NimBLE GATT peripheral that exposes the Nordic-UART-style MeshCore
// companion service. Peer (iPhone MeshCore app) writes opcode-prefixed
// frames to the RX characteristic; we feed them through
// companion_dispatch_frame() so the same opcode-14 handler that USB-CDC
// uses applies here too. Source tag is GPS_SRC_BLE.
//
// Pairing: Passkey Display Entry over LE Secure Connections. We inject the
// user-configured fixed passkey (ble_pin from Settings, default 000000) as the
// displayed code and surface it via the pairing callback; the iPhone prompts
// the user to type the same 6-digit code. Bonds are persisted in NVS so the
// second pairing is silent.

#pragma once

#include <stdbool.h>
#include <stdint.h>

// Bring up the NimBLE host stack via esp-hosted (so BLE physically runs on
// the C6) and register the companion service. Safe to call once at boot,
// after nvs_flash_init(). Returns true on success.
bool ble_companion_init(void);

// ── Diagnostics surface for the Paired-devices viewer (VIEW_BLE_DEVICES) ─────
// These wrap NimBLE state/store calls so the UI layer never has to include
// NimBLE headers (keeps the layer direction one-way, see Architecture.md).

// A bonded peer's identity address, in plain bytes (no NimBLE ble_addr_t).
typedef struct {
    uint8_t addr[6];    // identity address, display order (addr[0] = MSB)
    uint8_t addr_type;  // BLE_ADDR_* type code (0 public, 1 random, ...)
} ble_peer_t;

// Live BLE runtime status snapshot.
typedef struct {
    bool initialized;  // ble_companion_init() succeeded (NimBLE host is up)
    bool advertising;  // last ble_gap_adv_start() succeeded and no peer connected
    bool connected;    // a peer is currently connected
    int  bond_count;   // number of persisted bonds in NVS
} ble_status_t;

// Fill `out` with up to `max` bonded peers; returns the count written.
// Returns 0 if BLE never came up. `out` may be NULL when max == 0.
int ble_companion_bonded_peers(ble_peer_t* out, int max);

// Snapshot the current BLE runtime status into `out`.
void ble_companion_get_status(ble_status_t* out);

// Unpair every bonded peer (wipes bonds from NVS). Forces a fresh pairing on
// the next connect; use to recover from a stale bond that blocks re-pairing.
void ble_companion_clear_bonds(void);

// Called by the BLE module when NimBLE asks us to display a 6-digit passkey
// for the user to type on the iPhone. Default impl logs to ESP_LOG; PR-2b
// follow-up will replace this with a VIEW_BLE_PAIR modal on the display.
// Weak symbol so the UI code can override without changing the BLE module.
void ble_companion_show_passkey(uint32_t passkey);

// Called when pairing completes (success or fail). For the UI to dismiss
// the passkey modal. Weak symbol; default impl just logs.
void ble_companion_pair_done(bool success);
