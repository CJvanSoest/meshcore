// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// Thin wrappers around the ESP-IDF NVS APIs that collapse the
// open-get/set-commit-close ritual down to one call. The point isn't
// hiding NVS -- callers that need transactional batching keep using
// nvs_open/nvs_set_*/nvs_commit directly -- it's killing the copy-paste
// pattern for fields that are just "one scalar keyed under a namespace".

#pragma once

#include <stdbool.h>
#include <stdint.h>

// Read a single scalar from `ns` under `key`. If the key is missing or
// NVS itself isn't openable, *out stays at whatever the caller set
// (typically the default value). Returns true if a value was actually
// read so callers can distinguish "first boot" from "explicitly set".
bool nvs_load_u8 (const char *ns, const char *key, uint8_t  *out);
bool nvs_load_i8 (const char *ns, const char *key, int8_t   *out);

// Open RW, set+commit, close. Returns true if the write reached NVS.
// Errors are logged at WARN level so the caller doesn't need to.
bool nvs_save_u8 (const char *ns, const char *key, uint8_t  val);
bool nvs_save_i8 (const char *ns, const char *key, int8_t   val);
