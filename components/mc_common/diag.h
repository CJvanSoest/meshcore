// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// Diagnostics ring for the Toolbox packet log: a small mutex-protected ring of
// the most-recent radio frames (both directions), captured straight off the
// transport in radio.c. Storage only — the wire decode for display lives in
// the pure mc_proto/diag_decode helper, the UI in render_toolbox_log.c. The
// ring keeps filling regardless of the UI; "pause" is a UI-side freeze of what
// it re-reads, not a capture stop.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define DIAG_LOG_SIZE 64   // ring capacity (most-recent frames kept)
#define DIAG_RAW_MAX  176  // bytes stored per frame (a full ADVERT fits)

#define DIAG_DIR_RX   0
#define DIAG_DIR_TX   1
#define DIAG_RSSI_NONE 127  // sentinel: no RSSI/SNR (TX rows)

typedef struct {
    uint32_t now_ms;             // capture time (ms since boot)
    uint8_t  dir;                // DIAG_DIR_RX / DIAG_DIR_TX
    int8_t   rssi_dbm;           // RX only; DIAG_RSSI_NONE on TX
    int8_t   snr_db_x4;          // RX only (raw SNR, quarter-dB)
    uint8_t  full_len;           // true on-air length (may exceed raw_len)
    uint8_t  raw_len;            // bytes actually stored in raw[]
    uint8_t  raw[DIAG_RAW_MAX];  // leading on-air bytes (header + payload)
} diag_entry_t;

// Allocate the ring (PSRAM) + mutex. Idempotent; call once at boot.
void diag_init(void);

// Append a captured frame. Called from the radio RX task and the TX tail.
// Briefly takes the ring mutex; a no-op until diag_init() has run.
void diag_capture(uint8_t dir, const uint8_t *frame, uint8_t len,
                  int8_t rssi_dbm, int8_t snr_db_x4);

// Bulk-copy the whole ring into out (must hold DIAG_LOG_SIZE entries) under a
// brief lock — one memcpy, not a per-entry loop, so a colliding capture is not
// starved. Returns the valid-entry count and writes the ring write-head to
// *out_head; the caller walks newest-first as
//   out[(*out_head - 1 - i + 2 * DIAG_LOG_SIZE) % DIAG_LOG_SIZE], i = 0..count-1.
int diag_snapshot(diag_entry_t out[DIAG_LOG_SIZE], int *out_head);

// Total frames captured since boot (header counter; read without locking).
uint32_t diag_total(void);

// Drop all captured frames.
void diag_clear(void);
