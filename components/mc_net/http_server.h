// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// Tiny HTTP server that runs whenever WiFi is up. PR-2g step 1 ships a
// /test endpoint for round-trip verification; PR-2g step 2 adds /ping
// for the MeshMapper JSON payload, PR-2g step 3 upgrades to HTTPS.
//
// Auto-managed: a supervisor task starts the server when the WiFi link
// comes up and stops it on disconnect, mirroring wifi_keepalive's pattern.

#pragma once

#include <stdbool.h>
#include "esp_err.h"

// Spawn the supervisor task that starts/stops the HTTP server based on
// WiFi state. Call once at boot, after wifi_connection_init_stack().
void http_server_supervisor_start(void);

// Wipe NVS cert + key, generate a fresh self-signed cert, restart the
// server with it. Triggered from the Settings "Regen Cert" action row.
// After this the iPhone profile install must be repeated.
esp_err_t http_server_regen_cert(void);

// Live cert PEM (NUL-terminated) or NULL until first load. UI uses this
// to compute and display the SHA-256 fingerprint.
const char *http_server_cert_pem(void);
