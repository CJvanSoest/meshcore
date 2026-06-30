// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// VIEW_TOOLBOX_LOG — the live packet log (issue #3 a/b). Renders the diag ring
// (radio RX + TX frames) newest-first, one line per frame, in two modes:
//   HEX     — leading on-air bytes as a hex dump.
//   DISSECT — per-payload field breakdown (decoded in mc_proto/diag_decode).
// Pause freezes the displayed window while capture keeps running underneath.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "app_config.h"
#include "diag.h"
#include "diag_decode.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "render_internal.h"
#include "ui_state.h"

// PSRAM-resident snapshot of the ring, refreshed each live frame and frozen
// while paused so the user can read + scroll a stable window. s_snap holds the
// ring in raw order (the hex view needs the bytes); s_decoded holds each entry
// dissected once at refresh time so the render loop never re-decodes a row it
// already drew. s_snap_head locates the write head so both are walked
// newest-first. This is a second ~12 KB ring on top of diag.c's, plus the
// decoded cache, so the tool holds ~30 KB of PSRAM while open — fine on PSRAM.
static diag_entry_t*   s_snap       = NULL;
static diag_decoded_t* s_decoded    = NULL;
static int             s_snap_count = 0;
static int             s_snap_head  = 0;

static void ensure_snap(void) {
    if (s_snap != NULL) return;
    size_t bytes = sizeof(diag_entry_t) * DIAG_LOG_SIZE;
    s_snap       = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (s_snap == NULL) s_snap = malloc(bytes);
    size_t dbytes = sizeof(diag_decoded_t) * DIAG_LOG_SIZE;
    s_decoded     = heap_caps_malloc(dbytes, MALLOC_CAP_SPIRAM);
    if (s_decoded == NULL) s_decoded = malloc(dbytes);
}

// Raise a 2.5 s status toast from the export path. Centralised so every exit
// branch reports its outcome the same way.
static void log_export_toast(const char* msg) {
    snprintf(toast_text, sizeof(toast_text), "%s", msg);
    toast_duration_ms = 2500;
    toast_start_ms    = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

void toolbox_log_export_sd(void) {
    // Fresh snapshot independent of the render-time freeze, so an export grabs
    // the live ring even while the on-screen window is paused. ~12 KB on PSRAM
    // for the duration of the write, then freed.
    diag_entry_t* snap = heap_caps_malloc(sizeof(diag_entry_t) * DIAG_LOG_SIZE, MALLOC_CAP_SPIRAM);
    if (snap == NULL) snap = malloc(sizeof(diag_entry_t) * DIAG_LOG_SIZE);
    if (snap == NULL) {
        log_export_toast("Export failed: low memory");
        return;
    }
    int head  = 0;
    int count = diag_snapshot(snap, &head);

    // time(NULL) comes from the C6 RTC; unsynced it is still unique-enough to
    // not collide within a session (same basis as the coverage CSV name).
    mkdir("/sd/meshcore", 0775);
    mkdir("/sd/meshcore/log", 0775);
    char path[64];
    snprintf(path, sizeof(path), "/sd/meshcore/log/pkt_%lu.csv", (unsigned long)time(NULL));
    FILE* f = fopen(path, "w");
    if (f == NULL) {
        free(snap);
        log_export_toast("Export failed: no SD card?");
        return;
    }

    fputs(DIAG_CSV_HEADER "\n", f);
    char row[2 * DIAG_RAW_MAX + 96];  // 2 hex chars/byte + the fixed columns
    for (int i = 0; i < count; i++) {
        int                 ri = (head - 1 - i + 2 * DIAG_LOG_SIZE) % DIAG_LOG_SIZE;  // newest-first
        const diag_entry_t* e  = &snap[ri];
        diag_decoded_t      d;
        diag_decode(e->raw, e->raw_len, &d);
        diag_csv_row(e->now_ms, e->dir == DIAG_DIR_TX, e->rssi_dbm, e->snr_db_x4, e->full_len, e->raw, e->raw_len, &d,
                     row, sizeof(row));
        fputs(row, f);
        fputc('\n', f);
    }
    fclose(f);
    free(snap);

    // Sized past the worst-case bound the compiler infers for path[] so the
    // snprintf can't truncate; the 64-byte toast does the final clamp at runtime
    // (real paths are ~48 chars, so nothing is actually cut).
    char msg[96];
    snprintf(msg, sizeof(msg), "Saved %d pkts -> %s", count, path + 4);  // drop "/sd/"
    log_export_toast(msg);
}

// Build the right-hand detail string for one entry into `out`. Non-static so
// the LVGL list view (render_toolbox_log_lvgl) reuses the exact same formatting.
void toolbox_log_format_detail(const diag_entry_t* e, const diag_decoded_t* d, char* out, size_t cap) {
    if (toolbox_log_dissect) {
        if (!d->valid) {
            snprintf(out, cap, "(unparsed) %uB", e->full_len);
            return;
        }
        // Hops now has its own column, so it is dropped from this string.
        int p = snprintf(out, cap, "%s %uB ", diag_route_name(d->route), e->full_len);
        if (p < 0 || (size_t)p >= cap) return;
        if (d->has_pubkey) {
            p += snprintf(out + p, cap - p, "key:%02X%02X%02X %s", d->pubkey[0], d->pubkey[1], d->pubkey[2],
                          diag_role_name(d->role));
            if (d->has_name && (size_t)p < cap) {
                snprintf(out + p, cap - p, " \"%s\"", d->name);
            }
        } else if (d->has_hash) {
            snprintf(out + p, cap - p, "dst:%02X src:%02X", d->dest_hash, d->src_hash);
        }
    } else {
        // Hex dump of the leading on-air bytes (as many as fit a row).
        int p  = 0;
        int nb = e->raw_len > 30 ? 30 : e->raw_len;
        for (int b = 0; b < nb && p < (int)cap - 4; b++) {
            p += snprintf(out + p, cap - p, "%02X ", e->raw[b]);
        }
        if (e->full_len > e->raw_len && p < (int)cap - 4) {
            snprintf(out + p, cap - p, "...");
        }
    }
}

// Map a newest-first index to the raw ring slot in the frozen snapshot. Non-
// static so the LVGL view maps indices against the same captured head after
// calling toolbox_log_snapshot().
int toolbox_log_snap_ri(int newest_idx) {
    return (s_snap_head - 1 - newest_idx + 2 * DIAG_LOG_SIZE) % DIAG_LOG_SIZE;
}

// Refresh the frozen ring snapshot (unless paused) and hand back the single-
// source buffers the list/detail views walk. Mirrors the prep block at the top
// of render_toolbox_log() so the LVGL renderer never re-implements or duplicates
// the snapshot. Returns false if the PSRAM buffers could not be allocated.
bool toolbox_log_snapshot(const diag_entry_t** out_snap, const diag_decoded_t** out_decoded, int* out_count,
                          int* out_head) {
    ensure_snap();
    if (s_snap == NULL || s_decoded == NULL) return false;
    if (!toolbox_log_paused) {
        s_snap_count = diag_snapshot(s_snap, &s_snap_head);
        for (int i = 0; i < s_snap_count; i++) {
            int ri = (s_snap_head - 1 - i + 2 * DIAG_LOG_SIZE) % DIAG_LOG_SIZE;
            diag_decode(s_snap[ri].raw, s_snap[ri].raw_len, &s_decoded[ri]);
        }
    }
    *out_snap    = s_snap;
    *out_decoded = s_decoded;
    *out_count   = s_snap_count;
    *out_head    = s_snap_head;
    return true;
}
