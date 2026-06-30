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

// Called by the BLE module when NimBLE asks us to display a 6-digit passkey
// for the user to type on the iPhone. Default impl logs to ESP_LOG; PR-2b
// follow-up will replace this with a VIEW_BLE_PAIR modal on the display.
// Weak symbol so the UI code can override without changing the BLE module.
void ble_companion_show_passkey(uint32_t passkey);

// Called when pairing completes (success or fail). For the UI to dismiss
// the passkey modal. Weak symbol; default impl just logs.
void ble_companion_pair_done(bool success);
