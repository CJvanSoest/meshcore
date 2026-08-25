// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// Pure record-geometry + trim math for the on-disk chat log (history.c). Kept
// free of ESP-IDF / mbedtls / FreeRTOS so it can be unit-tested on the host
// (tests/test_history_trim.c) — the file walk and rewrite stay in history.c.

#pragma once

#include <stdint.h>

// Fixed on-disk header size of one history record (history_rec_hdr_t in
// history.c: magic[4] + plain_len(2) + flags(1) + reserved(1) + ts(4) +
// iv[16]). history.c static-asserts sizeof(hdr) against this.
#define HISTORY_REC_HDR_SIZE 28u

// On-disk byte size of one record given its plaintext length, matching the
// AES-CBC layout: header + plaintext padded up to the next 16-byte multiple.
// The pad is PKCS#7-style and always adds 1..16 bytes, so a 16-byte-aligned
// plaintext still grows by a full block.
uint32_t history_rec_disk_size(uint16_t plain_len);

// Given a file's record sizes in stored order (oldest first) and a byte cap,
// return the index of the first record to KEEP so the retained tail (the
// newest records) fits within `cap`. Records before the returned index are
// dropped. Returns 0 when everything already fits. Always keeps at least the
// single newest record, even if it alone exceeds the cap.
int history_trim_first_kept(const uint32_t* sizes, int count, uint32_t cap);
