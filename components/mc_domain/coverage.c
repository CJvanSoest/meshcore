// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "coverage.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nodes.h"  // node_list / node_count / node_mutex + role enum

#define COVERAGE_DIR "/sd/meshcore/coverage"

static coverage_result_t s_results[COVERAGE_MAX_RESULTS];
static int               s_result_count = 0;
static volatile bool     s_busy         = false;

// Armed TRACE-tag matcher: a single outstanding ping at a time (pings are
// sequential), set by the ping task and resolved by the RX path. On a hit we
// also stash the uplink SNR the trace returned and our downlink SNR.
static bool     s_tag_armed        = false;
static bool     s_tag_got          = false;
static uint32_t s_tag              = 0;
static int8_t   s_got_uplink_snr   = COVERAGE_SNR_NONE;
static int8_t   s_got_downlink_snr = COVERAGE_SNR_NONE;
static uint8_t  s_got_hops         = 0;

static char s_session_path[80];  // empty = no open session / no SD

static SemaphoreHandle_t s_mutex = NULL;

void coverage_init(void) {
    if (s_mutex == NULL) s_mutex = xSemaphoreCreateMutex();
}

static coverage_result_t* find_locked(const uint8_t pub[32]) {
    for (int i = 0; i < s_result_count; i++) {
        if (memcmp(s_results[i].pub_prefix, pub, 6) == 0) return &s_results[i];
    }
    return NULL;
}

static coverage_result_t* find_or_add_locked(const uint8_t pub[32]) {
    coverage_result_t* r = find_locked(pub);
    if (r) return r;
    if (s_result_count >= COVERAGE_MAX_RESULTS) return NULL;
    r = &s_results[s_result_count++];
    memset(r, 0, sizeof(*r));
    memcpy(r->pub_prefix, pub, 6);
    r->status      = COVERAGE_NONE;
    r->best_snr_x4 = COVERAGE_SNR_NONE;
    return r;
}

static coverage_status_t classify(uint8_t attempts, uint8_t acks) {
    if (attempts < COVERAGE_PINGS) return COVERAGE_TESTING;
    if (acks == 0) return COVERAGE_FAIL;
    if (acks >= COVERAGE_PINGS) return COVERAGE_OK;
    return COVERAGE_PARTIAL;
}

void coverage_session_reset(void) {
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    s_result_count = 0;
    // New SD log file for this area test. time(NULL) comes from the C6 RTC; if
    // unsynced it is still monotonic-enough within a session to be unique.
    mkdir("/sd/meshcore", 0775);
    mkdir(COVERAGE_DIR, 0775);
    snprintf(s_session_path, sizeof(s_session_path), "%s/cov_%lu.csv", COVERAGE_DIR, (unsigned long)time(NULL));
    FILE* f = fopen(s_session_path, "w");
    if (f) {
        fputs("ts_unix,repeater,pubkey,lat_e6,lon_e6,attempt,reachable,rtt_ms,uplink_snr_db,downlink_snr_db\n", f);
        fclose(f);
    } else {
        s_session_path[0] = '\0';  // no SD: keep in-RAM results only
    }
    xSemaphoreGive(s_mutex);
}

void coverage_set_testing(const uint8_t pub[32]) {
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    coverage_result_t* r = find_or_add_locked(pub);
    if (r) {
        r->attempts    = 0;
        r->acks        = 0;
        r->status      = COVERAGE_TESTING;
        r->best_snr_x4 = COVERAGE_SNR_NONE;
    }
    xSemaphoreGive(s_mutex);
}

void coverage_record(const uint8_t pub[32], bool reachable, int8_t snr_x4) {
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    coverage_result_t* r = find_or_add_locked(pub);
    if (r) {
        if (r->attempts < 255) r->attempts++;
        if (reachable && r->acks < 255) r->acks++;
        if (reachable && snr_x4 != COVERAGE_SNR_NONE &&
            (r->best_snr_x4 == COVERAGE_SNR_NONE || snr_x4 > r->best_snr_x4)) {
            r->best_snr_x4 = snr_x4;
        }
        r->status = classify(r->attempts, r->acks);
    }
    xSemaphoreGive(s_mutex);
}

bool coverage_lookup(const uint8_t pub[32], coverage_result_t* out) {
    bool found = false;
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    coverage_result_t* r = find_locked(pub);
    if (r) {
        if (out) *out = *r;
        found = true;
    }
    xSemaphoreGive(s_mutex);
    return found;
}

void coverage_set_busy(bool busy) {
    s_busy = busy;
}
bool coverage_busy(void) {
    return s_busy;
}

void coverage_arm_tag(uint32_t tag) {
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
    s_tag              = tag;
    s_tag_armed        = true;
    s_tag_got          = false;
    s_got_uplink_snr   = COVERAGE_SNR_NONE;
    s_got_downlink_snr = COVERAGE_SNR_NONE;
    s_got_hops         = 0;
    xSemaphoreGive(s_mutex);
}

bool coverage_note_tag(uint32_t tag, int8_t uplink_snr_x4, int8_t downlink_snr_x4, uint8_t hops) {
    bool matched = false;
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return false;
    if (s_tag_armed && tag == s_tag) {
        s_tag_got          = true;
        s_got_uplink_snr   = uplink_snr_x4;
        s_got_downlink_snr = downlink_snr_x4;
        s_got_hops         = hops;
        matched            = true;
    }
    xSemaphoreGive(s_mutex);
    return matched;
}

bool coverage_take_tag(int8_t* uplink_snr_x4, int8_t* downlink_snr_x4, uint8_t* hops) {
    bool got = false;
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    if (s_tag_got) {
        got         = true;
        s_tag_got   = false;
        s_tag_armed = false;
        if (uplink_snr_x4) *uplink_snr_x4 = s_got_uplink_snr;
        if (downlink_snr_x4) *downlink_snr_x4 = s_got_downlink_snr;
        if (hops) *hops = s_got_hops;
    }
    xSemaphoreGive(s_mutex);
    return got;
}

// Haversine distance in metres between two e6-degree coordinates.
static float cov_distance_m(int32_t lat1_e6, int32_t lon1_e6, int32_t lat2_e6, int32_t lon2_e6) {
    const double R = 6371000.0, D = M_PI / 180.0 / 1e6;
    double       p1 = lat1_e6 * D, p2 = lat2_e6 * D;
    double       dp = (lat2_e6 - lat1_e6) * D, dl = (lon2_e6 - lon1_e6) * D;
    double       a = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
    return (float)(R * 2 * atan2(sqrt(a), sqrt(1 - a)));
}

int coverage_collect_repeaters(coverage_repeater_t* out, int max, int32_t ref_lat_e6, int32_t ref_lon_e6,
                               bool ref_valid, uint32_t max_dist_m) {
    if (out == NULL || max <= 0) return 0;
    if (node_mutex == NULL || xSemaphoreTake(node_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return 0;
    int n = 0;
    for (int i = 0; i < node_count && n < max; i++) {
        const node_entry_t* e = &node_list[i];
        if (!e->active || e->role != MESHCORE_DEVICE_ROLE_REPEATER) continue;
        // Distance filter: drop repeaters known to be beyond max_dist_m. Keep
        // ones without a position (unknown distance) so a nearby node is never hidden.
        if (ref_valid && max_dist_m > 0 && e->position_valid &&
            cov_distance_m(ref_lat_e6, ref_lon_e6, e->lat, e->lon) > (float)max_dist_m) {
            continue;
        }
        memcpy(out[n].pub, e->pub_key, 32);
        strncpy(out[n].name, e->name, sizeof(out[n].name) - 1);
        out[n].name[sizeof(out[n].name) - 1] = '\0';
        out[n].lat_e6                        = e->lat;
        out[n].lon_e6                        = e->lon;
        out[n].pos_valid                     = e->position_valid;
        n++;
    }
    xSemaphoreGive(node_mutex);
    return n;
}

// Format an SNR (quarter-dB) into buf, or empty when it is the absent sentinel.
static void fmt_snr(char* buf, size_t cap, int8_t snr_x4) {
    if (snr_x4 == COVERAGE_SNR_NONE) {
        buf[0] = '\0';
    } else {
        // One decimal: snr_x4 is in quarter-dB, so value = snr_x4 / 4.
        int whole = snr_x4 / 4;
        int frac  = (snr_x4 < 0 ? -snr_x4 : snr_x4) % 4 * 25;  // .00/.25/.50/.75 -> 0/25/50/75
        snprintf(buf, cap, "%d.%02d", whole, frac);
    }
}

void coverage_log(const uint8_t pub[32], const char* name, int32_t lat_e6, int32_t lon_e6, bool gps_valid, int attempt,
                  bool reachable, uint32_t rtt_ms, int8_t uplink_snr_x4, int8_t downlink_snr_x4) {
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    if (s_session_path[0]) {
        FILE* f = fopen(s_session_path, "a");
        if (f) {
            char lat[16] = "", lon[16] = "";
            if (gps_valid) {
                snprintf(lat, sizeof(lat), "%ld", (long)lat_e6);
                snprintf(lon, sizeof(lon), "%ld", (long)lon_e6);
            }
            char up[12], down[12];
            fmt_snr(up, sizeof(up), uplink_snr_x4);
            fmt_snr(down, sizeof(down), downlink_snr_x4);
            fprintf(f, "%lu,%s,%02X%02X%02X%02X,%s,%s,%d,%d,%lu,%s,%s\n", (unsigned long)time(NULL), name ? name : "",
                    pub[0], pub[1], pub[2], pub[3], lat, lon, attempt, reachable ? 1 : 0, (unsigned long)rtt_ms, up,
                    down);
            fclose(f);
        }
    }
    xSemaphoreGive(s_mutex);
}
