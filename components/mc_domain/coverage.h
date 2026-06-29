// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// Coverage test (Toolbox iteration 2, #3): per-repeater reachability results
// for one field-test session + a TRACE-tag matcher. The ping is an upstream
// MeshCore TRACE (PAYLOAD_TYPE_TRACE), the only reachability probe a repeater
// answers without an admin login; the ping controller lives in mc_rx, and this
// module owns the result model, the armed-tag state the RX path notifies (with
// the per-hop / local SNR the trace returned), and the SD CSV log. Mutex-protected.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define COVERAGE_MAX_RESULTS 64     // repeaters tracked per session
#define COVERAGE_RADIUS_M    15000  // list filter: only repeaters within 15 km of us
#define COVERAGE_PINGS       3      // pings per repeater

typedef enum {
    COVERAGE_NONE = 0,  // not tested yet
    COVERAGE_TESTING,   // ping run in progress
    COVERAGE_FAIL,      // 0 acks  (red)
    COVERAGE_PARTIAL,   // 1..n-1  (orange)
    COVERAGE_OK,        // all acks (green)
} coverage_status_t;

#define COVERAGE_SNR_NONE 127  // sentinel: no SNR captured for this result

typedef struct {
    uint8_t           pub_prefix[6];  // first 6 bytes of the repeater pubkey
    uint8_t           attempts;       // pings sent this session
    uint8_t           acks;           // traces that returned (reachable)
    coverage_status_t status;
    int8_t            best_snr_x4;  // best downlink SNR seen (quarter-dB), or COVERAGE_SNR_NONE
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
int coverage_collect_repeaters(coverage_repeater_t* out, int max, int32_t ref_lat_e6, int32_t ref_lon_e6,
                               bool ref_valid, uint32_t max_dist_m);

// Create the mutex. Idempotent; call once at boot.
void coverage_init(void);

// Start a new session: clear results + open a fresh SD log file with a header.
void coverage_session_reset(void);

// ── Result model (keyed by full pubkey; a 6-byte prefix is stored) ───────────
void coverage_set_testing(const uint8_t pub[32]);
// Bump attempts, count a returned trace (reachable), and fold in the downlink
// SNR (quarter-dB; COVERAGE_SNR_NONE when none), then recompute status.
void coverage_record(const uint8_t pub[32], bool reachable, int8_t snr_x4);
bool coverage_lookup(const uint8_t pub[32], coverage_result_t* out);

// Controller-busy flag (a ping run is active). UI reads it to block re-entry.
void coverage_set_busy(bool busy);
bool coverage_busy(void);

// ── TRACE-tag matcher (RX path <-> ping task), independent of the chat ring ──
void coverage_arm_tag(uint32_t tag);  // ping task: expect this trace tag back
// RX path: report a returned trace by tag, with the uplink SNR the trace
// collected (first hop's, quarter-dB) and our downlink SNR of the return frame.
// Returns true when the tag matched the armed probe.
bool coverage_note_tag(uint32_t tag, int8_t uplink_snr_x4, int8_t downlink_snr_x4, uint8_t hops);
// ping task: poll + clear the hit; fills the two SNRs on a hit.
bool coverage_take_tag(int8_t* uplink_snr_x4, int8_t* downlink_snr_x4, uint8_t* hops);

// Append one CSV row to the session log (no-op if SD is unavailable).
void coverage_log(const uint8_t pub[32], const char* name, int32_t lat_e6, int32_t lon_e6, bool gps_valid, int attempt,
                  bool reachable, uint32_t rtt_ms, int8_t uplink_snr_x4, int8_t downlink_snr_x4);
