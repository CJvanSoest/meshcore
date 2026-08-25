// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// HMAC-SHA256 built on the still-public mbedtls MD *hash* API. mbedtls 4.1
// (ESP-IDF 6) dropped the mbedtls_md_hmac* convenience helpers, but keeps the
// plain mbedtls_md hash primitive (hardware-accelerated on the ESP32), so we
// compute HMAC from that per RFC 2104. One-shot only; the one former streaming
// call site concatenates its two chunks into a small buffer instead.

#pragma once

#include <stddef.h>
#include <stdint.h>

// out receives the 32-byte HMAC-SHA256 of msg under key.
void mc_hmac_sha256(const uint8_t* key, size_t key_len, const uint8_t* msg, size_t msg_len, uint8_t out[32]);
