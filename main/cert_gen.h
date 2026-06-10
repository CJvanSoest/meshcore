// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// On-device self-signed TLS cert generation + NVS storage.
//
// Replaces the static main/certs/server.{crt,key} blobs that used to be
// EMBED_TXTFILES'd into the binary. Each badge generates its own ECDSA
// P-256 keypair on first boot, builds a self-signed X.509 cert with
// SAN = DNS:tanmatsu.local + DNS:tanmatsu, and persists both to NVS so
// they survive a reflash. The cert is never regenerated automatically;
// the user can force a fresh one via the Settings UI.

#pragma once

#include <stddef.h>
#include "esp_err.h"

// PEM buffers are sized for ECDSA P-256: cert ~600 B, key ~250 B. The
// helpers below all expect caller-owned buffers of these sizes.
#define CERT_GEN_PEM_CERT_MAX 2048
#define CERT_GEN_PEM_KEY_MAX  1024

// Load cert+key PEM from NVS (namespace "http_cert"). Returns ESP_OK if
// both blobs are present and fit the buffers; ESP_ERR_NVS_NOT_FOUND if
// either is missing (caller should generate). Output strings are NUL-
// terminated.
esp_err_t cert_gen_nvs_load(char *cert_pem, size_t cert_cap,
                            char *key_pem,  size_t key_cap);

// Save cert+key PEM to NVS. Strings must be NUL-terminated PEM.
esp_err_t cert_gen_nvs_save(const char *cert_pem, const char *key_pem);

// Wipe NVS cert+key. Next call to cert_gen_load_or_create will regenerate.
esp_err_t cert_gen_nvs_clear(void);

// Generate a fresh ECDSA P-256 keypair + self-signed X.509 cert with
// CN=tanmatsu.local, SAN=DNS:tanmatsu.local,DNS:tanmatsu, 10-year
// validity. Caller-allocated PEM buffers. Returns ESP_OK or an esp_err_t
// translated from the underlying mbedTLS error.
esp_err_t cert_gen_self_signed(char *cert_pem, size_t cert_cap,
                               char *key_pem,  size_t key_cap);

// Convenience: load from NVS, or generate + save if absent. After return,
// cert_pem + key_pem hold the live values regardless of which path ran.
esp_err_t cert_gen_load_or_create(char *cert_pem, size_t cert_cap,
                                  char *key_pem,  size_t key_cap);

// SHA-256 fingerprint of the DER form of the cert, as a 64-char lowercase
// hex string (NUL-terminated, so hex_cap >= 65). Used by the Settings UI
// row so the user can pin the cert in OwnTracks / verify install on iOS.
esp_err_t cert_gen_fingerprint_hex(const char *cert_pem,
                                   char *hex_out, size_t hex_cap);
