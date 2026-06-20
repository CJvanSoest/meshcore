// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// Coverage test (Toolbox iteration 2, #3): per-repeater reachability results
// for one field-test session + an ACK matcher decoupled from the chat ring, so
// a coverage ping never pollutes DM history. The ping controller lives in
// mc_rx (it composes DMs); this module owns the result model, the armed-ACK
// state the RX path notifies, and the SD CSV log. Mutex-protected.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define COVERAGE_MAX_RESULTS 32  // repeaters tracked per session
#define COVERAGE_PINGS       3   // pings per repeater

typedef enum {
    COVERAGE_NONE = 0,  // not tested yet
    COVERAGE_TESTING,   // ping run in progress
    COVERAGE_FAIL,      // 0 acks  (red)
    COVERAGE_PARTIAL,   // 1..n-1  (orange)
    COVERAGE_OK,        // all acks (green)
} coverage_status_t;

typedef struct {
    uint8_t           pub_prefix[6];  // first 6 bytes of the repeater pubkey
    uint8_t           attempts;       // pings sent this session
    uint8_t           acks;           // acks received
    coverage_status_t status;
} coverage_result_t;

// A discovered repeater, snapshotted for the coverage list (so the view and
// the input handler agree on cursor -> repeater without each walking node_list).
typedef struct {
    uint8_t pub[32];
    char    name[33];
    int32_t lat_e6;
    int32_t lon_e6;
    bool    pos_valid;
} coverage_repeater_t;

// Snapshot discovered repeaters (role == REPEATER) into out[]; returns count.
// Briefly holds node_mutex. Safe to call from the UI task.
int coverage_collect_repeaters(coverage_repeater_t *out, int max);

// Create the mutex. Idempotent; call once at boot.
void coverage_init(void);

// Start a new session: clear results + open a fresh SD log file with a header.
void coverage_session_reset(void);

// ── Result model (keyed by full pubkey; a 6-byte prefix is stored) ───────────
void coverage_set_testing(const uint8_t pub[32]);
void coverage_record(const uint8_t pub[32], bool ack);  // bump + recompute status
bool coverage_lookup(const uint8_t pub[32], coverage_result_t *out);

// Controller-busy flag (a ping run is active). UI reads it to block re-entry.
void coverage_set_busy(bool busy);
bool coverage_busy(void);

// ── ACK matcher (RX path <-> ping task), independent of the chat ring ────────
void coverage_arm_ack(const uint8_t crc[4]);   // ping task: expect this CRC
bool coverage_note_ack(const uint8_t crc[4]);  // RX path: report a seen ACK CRC
bool coverage_take_ack(void);                  // ping task: poll + clear the hit

// Append one CSV row to the session log (no-op if SD is unavailable).
void coverage_log(const uint8_t pub[32], const char *name,
                  int32_t lat_e6, int32_t lon_e6, bool gps_valid,
                  int attempt, bool ack, uint32_t rtt_ms);
