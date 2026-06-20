// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "coverage.h"

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

// Armed-ACK matcher: a single outstanding ping CRC at a time (pings are
// sequential), set by the ping task and resolved by the RX path.
static bool    s_ack_armed = false;
static bool    s_ack_got   = false;
static uint8_t s_ack_crc[4];

static char s_session_path[80];  // empty = no open session / no SD

static SemaphoreHandle_t s_mutex = NULL;

void coverage_init(void) {
    if (s_mutex == NULL) s_mutex = xSemaphoreCreateMutex();
}

static coverage_result_t *find_locked(const uint8_t pub[32]) {
    for (int i = 0; i < s_result_count; i++) {
        if (memcmp(s_results[i].pub_prefix, pub, 6) == 0) return &s_results[i];
    }
    return NULL;
}

static coverage_result_t *find_or_add_locked(const uint8_t pub[32]) {
    coverage_result_t *r = find_locked(pub);
    if (r) return r;
    if (s_result_count >= COVERAGE_MAX_RESULTS) return NULL;
    r = &s_results[s_result_count++];
    memset(r, 0, sizeof(*r));
    memcpy(r->pub_prefix, pub, 6);
    r->status = COVERAGE_NONE;
    return r;
}

static coverage_status_t classify(uint8_t attempts, uint8_t acks) {
    if (attempts < COVERAGE_PINGS) return COVERAGE_TESTING;
    if (acks == 0)                 return COVERAGE_FAIL;
    if (acks >= COVERAGE_PINGS)    return COVERAGE_OK;
    return COVERAGE_PARTIAL;
}

void coverage_session_reset(void) {
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    s_result_count = 0;
    // New SD log file for this area test. time(NULL) comes from the C6 RTC; if
    // unsynced it is still monotonic-enough within a session to be unique.
    mkdir("/sd/meshcore", 0775);
    mkdir(COVERAGE_DIR, 0775);
    snprintf(s_session_path, sizeof(s_session_path), "%s/cov_%lu.csv",
             COVERAGE_DIR, (unsigned long)time(NULL));
    FILE *f = fopen(s_session_path, "w");
    if (f) {
        fputs("ts_unix,repeater,pubkey,lat_e6,lon_e6,attempt,ack,rtt_ms\n", f);
        fclose(f);
    } else {
        s_session_path[0] = '\0';  // no SD: keep in-RAM results only
    }
    xSemaphoreGive(s_mutex);
}

void coverage_set_testing(const uint8_t pub[32]) {
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    coverage_result_t *r = find_or_add_locked(pub);
    if (r) {
        r->attempts = 0;
        r->acks     = 0;
        r->status   = COVERAGE_TESTING;
    }
    xSemaphoreGive(s_mutex);
}

void coverage_record(const uint8_t pub[32], bool ack) {
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    coverage_result_t *r = find_or_add_locked(pub);
    if (r) {
        if (r->attempts < 255) r->attempts++;
        if (ack && r->acks < 255) r->acks++;
        r->status = classify(r->attempts, r->acks);
    }
    xSemaphoreGive(s_mutex);
}

bool coverage_lookup(const uint8_t pub[32], coverage_result_t *out) {
    bool found = false;
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    coverage_result_t *r = find_locked(pub);
    if (r) {
        if (out) *out = *r;
        found = true;
    }
    xSemaphoreGive(s_mutex);
    return found;
}

void coverage_set_busy(bool busy) { s_busy = busy; }
bool coverage_busy(void) { return s_busy; }

void coverage_arm_ack(const uint8_t crc[4]) {
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
    memcpy(s_ack_crc, crc, 4);
    s_ack_armed = true;
    s_ack_got   = false;
    xSemaphoreGive(s_mutex);
}

bool coverage_note_ack(const uint8_t crc[4]) {
    bool matched = false;
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return false;
    if (s_ack_armed && memcmp(s_ack_crc, crc, 4) == 0) {
        s_ack_got = true;
        matched   = true;
    }
    xSemaphoreGive(s_mutex);
    return matched;
}

bool coverage_take_ack(void) {
    bool got = false;
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    if (s_ack_got) {
        got         = true;
        s_ack_got   = false;
        s_ack_armed = false;
    }
    xSemaphoreGive(s_mutex);
    return got;
}

int coverage_collect_repeaters(coverage_repeater_t *out, int max) {
    if (out == NULL || max <= 0) return 0;
    if (node_mutex == NULL || xSemaphoreTake(node_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return 0;
    int n = 0;
    for (int i = 0; i < node_count && n < max; i++) {
        const node_entry_t *e = &node_list[i];
        if (!e->active || e->role != MESHCORE_DEVICE_ROLE_REPEATER) continue;
        memcpy(out[n].pub, e->pub_key, 32);
        strncpy(out[n].name, e->name, sizeof(out[n].name) - 1);
        out[n].name[sizeof(out[n].name) - 1] = '\0';
        out[n].lat_e6    = e->lat;
        out[n].lon_e6    = e->lon;
        out[n].pos_valid = e->position_valid;
        n++;
    }
    xSemaphoreGive(node_mutex);
    return n;
}

void coverage_log(const uint8_t pub[32], const char *name,
                  int32_t lat_e6, int32_t lon_e6, bool gps_valid,
                  int attempt, bool ack, uint32_t rtt_ms) {
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    if (s_session_path[0]) {
        FILE *f = fopen(s_session_path, "a");
        if (f) {
            char lat[16] = "", lon[16] = "";
            if (gps_valid) {
                snprintf(lat, sizeof(lat), "%ld", (long)lat_e6);
                snprintf(lon, sizeof(lon), "%ld", (long)lon_e6);
            }
            fprintf(f, "%lu,%s,%02X%02X%02X%02X,%s,%s,%d,%d,%lu\n",
                    (unsigned long)time(NULL), name ? name : "",
                    pub[0], pub[1], pub[2], pub[3], lat, lon,
                    attempt, ack ? 1 : 0, (unsigned long)rtt_ms);
            fclose(f);
        }
    }
    xSemaphoreGive(s_mutex);
}
