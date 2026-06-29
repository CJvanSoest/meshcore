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
#include "pax_gfx.h"
#include "pax_text.h"
#include "render.h"
#include "render_internal.h"
#include "ui_state.h"

#define LOG_HEADER_H 44
#define LOG_COLHDR_H 20
#define LOG_FOOTER_H 38
#define LOG_ROW_H    24

// Column x-positions (px). Kept narrow so the detail/hex column gets the rest.
#define COL_TS     8
#define COL_DIR    62
#define COL_TYPE   96
#define COL_RSSI   166
#define COL_SNR    212
#define COL_HOPS   268
#define COL_DETAIL 312

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

static void log_header(int w) {
    pax_simple_rect(&fb, COL_HEADER, 0, 0, w, LOG_HEADER_H);
    pax_simple_rect(&fb, COL_PAGER_ACCENT, 0, LOG_HEADER_H - 1, w, 1);
    int ty = (LOG_HEADER_H - TXT_TAB) / 2;
    pax_draw_text(&fb, COL_WHITE, FONT, TXT_TAB, 10, ty, "Packet Log");

    char meta[48];
    snprintf(meta, sizeof(meta), "%s  %lu pkts", toolbox_log_dissect ? "DISSECT" : "HEX", (unsigned long)diag_total());
    pax_vec2f sz = pax_text_size(FONT, TXT_SMALL, meta);
    int       mx = w - (int)sz.x - 12;
    if (toolbox_log_paused) {
        const char* p    = "PAUSED";
        pax_vec2f   psz  = pax_text_size(FONT, TXT_SMALL, p);
        mx              -= (int)psz.x + 12;
        pax_draw_text(&fb, COL_RED, FONT, TXT_SMALL, w - (int)psz.x - 12, (LOG_HEADER_H - TXT_SMALL) / 2, p);
    }
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, mx, (LOG_HEADER_H - TXT_SMALL) / 2, meta);
}

// Column-name strip drawn between the title header and the data rows so each
// row's fields read against a label. Same COL_* x-positions as the rows.
static void log_col_headers(int w, int top) {
    int ly = top + (LOG_COLHDR_H - TXT_TINY) / 2;
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_TINY, COL_TS, ly, "Time");
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_TINY, COL_DIR, ly, "Dir");
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_TINY, COL_TYPE, ly, "Type");
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_TINY, COL_RSSI, ly, "RSSI");
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_TINY, COL_SNR, ly, "SNR");
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_TINY, COL_HOPS, ly, "Hops");
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_TINY, COL_DETAIL, ly, "Detail");
    pax_simple_rect(&fb, COL_HEADER, 0, top + LOG_COLHDR_H - 1, w, 1);
}

// Build the right-hand detail string for one entry into `out`.
static void format_detail(const diag_entry_t* e, const diag_decoded_t* d, char* out, size_t cap) {
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

// Map a newest-first index to the raw ring slot in the frozen snapshot.
static int snap_ri(int newest_idx) {
    return (s_snap_head - 1 - newest_idx + 2 * DIAG_LOG_SIZE) % DIAG_LOG_SIZE;
}

// Full-screen breakdown of one captured frame: every field plus the complete
// 32-byte public key and the raw bytes, in mono so the hex lines up and i/l/1
// stay distinct. Reached with Enter on a selected row; the red X returns to the list.
static void render_log_detail(int w, int h, const diag_entry_t* e, const diag_decoded_t* d) {
    pax_simple_rect(&fb, COL_HEADER, 0, 0, w, LOG_HEADER_H);
    pax_simple_rect(&fb, COL_PAGER_ACCENT, 0, LOG_HEADER_H - 1, w, 1);
    pax_draw_text(&fb, COL_WHITE, FONT, TXT_TAB, 10, (LOG_HEADER_H - TXT_TAB) / 2, "Packet Detail");

    int  x  = 12;
    int  y  = LOG_HEADER_H + 10;
    bool rx = (e->dir == DIAG_DIR_RX);
    char line[96];

    snprintf(line, sizeof(line), "%s   %s   %s", rx ? "RX" : "TX", d->valid ? diag_type_name(d->ptype) : "?",
             d->valid ? diag_route_name(d->route) : "");
    pax_draw_text(&fb, rx ? COL_GREEN : COL_AMBER, FONT, TXT_BODY, x, y, line);
    y += TXT_BODY + 6;

    snprintf(line, sizeof(line), "time %lus   len %uB   hops %u", (unsigned long)(e->now_ms / 1000), e->full_len,
             d->hops);
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, x, y, line);
    y += TXT_SMALL + 4;

    if (rx && e->rssi_dbm != DIAG_RSSI_NONE) {
        int sw = e->snr_db_x4 / 4, sf = (e->snr_db_x4 < 0 ? -e->snr_db_x4 : e->snr_db_x4) % 4 * 25;
        snprintf(line, sizeof(line), "RSSI %d dBm   SNR %d.%02d dB", e->rssi_dbm, sw, sf);
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, x, y, line);
        y += TXT_SMALL + 4;
    }

    if (d->has_pubkey) {
        pax_draw_text(&fb, COL_AMBER, FONT, TXT_SMALL, x, y, "Public key:");
        y += TXT_SMALL + 2;
        for (int half = 0; half < 2; half++) {
            char hex[40];
            int  p = 0;
            for (int b = 0; b < 16 && p < (int)sizeof(hex); b++) {
                int n = snprintf(hex + p, sizeof(hex) - p, "%02X", d->pubkey[half * 16 + b]);
                if (n < 0 || n >= (int)sizeof(hex) - p) break;  // truncated/full: stop before size_t underflows
                p += n;
            }
            pax_draw_text(&fb, COL_GREEN, MONO, TXT_SMALL, x + 8, y, hex);
            y += TXT_SMALL + 2;
        }
        snprintf(line, sizeof(line), "role %s   name %s", diag_role_name(d->role), d->has_name ? d->name : "-");
        pax_draw_text(&fb, COL_PAGER_TEXT, FONT, TXT_SMALL, x, y, line);
        y += TXT_SMALL + 6;
    } else if (d->has_hash) {
        snprintf(line, sizeof(line), "dst-hash %02X   src-hash %02X", d->dest_hash, d->src_hash);
        pax_draw_text(&fb, COL_PAGER_TEXT, FONT, TXT_SMALL, x, y, line);
        y += TXT_SMALL + 6;
    }

    pax_draw_text(&fb, COL_AMBER, FONT, TXT_SMALL, x, y, "Bytes (on-air):");
    y      += TXT_SMALL + 2;
    int fy  = h - LOG_FOOTER_H;
    for (int off = 0; off < e->raw_len && y < fy - TXT_SMALL; off += 16) {
        char hex[56];
        int  p = 0;
        for (int b = off; b < off + 16 && b < e->raw_len && p < (int)sizeof(hex); b++) {
            int n = snprintf(hex + p, sizeof(hex) - p, "%02X ", e->raw[b]);
            if (n < 0 || n >= (int)sizeof(hex) - p) break;  // truncated/full: stop before size_t underflows
            p += n;
        }
        pax_draw_text(&fb, COL_PAGER_TEXT, MONO, TXT_SMALL, x + 8, y, hex);
        y += TXT_SMALL + 2;
    }

    pax_simple_rect(&fb, COL_HEADER, 0, fy, w, LOG_FOOTER_H);
    pax_simple_rect(&fb, COL_PAGER_ACCENT, 0, fy, w, 1);
    render_back_hint(10, fy + (LOG_FOOTER_H - TXT_SMALL) / 2, ": back to list", TXT_SMALL);
}

void render_toolbox_log(void) {
    int w = (int)pax_buf_get_width(&fb);
    int h = (int)pax_buf_get_height(&fb);

    pax_background(&fb, COL_BLACK);
    ensure_snap();

    // Refresh the live window unless frozen. The buffers may be NULL if a PSRAM
    // alloc failed; bail to a message rather than dereferencing them.
    if (s_snap == NULL || s_decoded == NULL) {
        log_header(w);
        pax_draw_text(&fb, COL_RED, FONT, TXT_SMALL, 12, LOG_HEADER_H + 12, "packet log unavailable (no memory)");
        return;
    }
    if (!toolbox_log_paused) {
        s_snap_count = diag_snapshot(s_snap, &s_snap_head);
        // Dissect each captured entry once, here, so the render loop reads a
        // cached result instead of re-decoding every visible row each frame.
        // Decode runs on the raw_len-capped prefix (DIAG_RAW_MAX = 176B): a
        // longer frame dissects a display-only truncation; header fields stay
        // complete.
        for (int i = 0; i < s_snap_count; i++) {
            int ri = (s_snap_head - 1 - i + 2 * DIAG_LOG_SIZE) % DIAG_LOG_SIZE;
            diag_decode(s_snap[ri].raw, s_snap[ri].raw_len, &s_decoded[ri]);
        }
    }

    // Clamp the selection cursor, then show its full breakdown if requested.
    if (toolbox_log_cursor >= s_snap_count) toolbox_log_cursor = s_snap_count - 1;
    if (toolbox_log_cursor < 0) toolbox_log_cursor = 0;
    if (toolbox_log_detail && s_snap_count > 0) {
        int ri = snap_ri(toolbox_log_cursor);
        render_log_detail(w, h, &s_snap[ri], &s_decoded[ri]);
        return;
    }

    int rows_y0  = LOG_HEADER_H + 4 + LOG_COLHDR_H;
    int avail_h  = h - rows_y0 - LOG_FOOTER_H;
    int rows_vis = avail_h / LOG_ROW_H;
    if (rows_vis < 1) rows_vis = 1;

    // Scroll follows the cursor so the selected row stays visible.
    if (toolbox_log_cursor < toolbox_log_scroll) toolbox_log_scroll = toolbox_log_cursor;
    if (toolbox_log_cursor >= toolbox_log_scroll + rows_vis) toolbox_log_scroll = toolbox_log_cursor - rows_vis + 1;
    int max_scroll = s_snap_count - rows_vis;
    if (max_scroll < 0) max_scroll = 0;
    if (toolbox_log_scroll > max_scroll) toolbox_log_scroll = max_scroll;
    if (toolbox_log_scroll < 0) toolbox_log_scroll = 0;

    log_header(w);
    log_col_headers(w, LOG_HEADER_H + 4);

    if (s_snap_count == 0) {
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 12, rows_y0 + 8, "Waiting for radio traffic...");
    }

    for (int i = 0; i < rows_vis; i++) {
        int si = toolbox_log_scroll + i;
        if (si >= s_snap_count) break;
        // Newest-first walk over the raw-order snapshot via the captured head;
        // the matching dissection was cached at refresh time.
        int                   ri = snap_ri(si);
        const diag_entry_t*   e  = &s_snap[ri];
        const diag_decoded_t* d  = &s_decoded[ri];
        int                   ry = rows_y0 + i * LOG_ROW_H;

        if (si == toolbox_log_cursor) {
            pax_simple_rect(&fb, COL_PANEL, 0, ry, w, LOG_ROW_H);
            pax_simple_rect(&fb, COL_PAGER_ACCENT, 0, ry, 3, LOG_ROW_H);
        } else if (i & 1) {
            pax_simple_rect(&fb, COL_HEADER, 0, ry, w, LOG_ROW_H);
        }

        int ty = ry + (LOG_ROW_H - TXT_TINY) / 2;

        char ts[12];
        snprintf(ts, sizeof(ts), "%lus", (unsigned long)(e->now_ms / 1000));
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_TINY, COL_TS, ty, ts);

        bool rx = (e->dir == DIAG_DIR_RX);
        pax_draw_text(&fb, rx ? COL_GREEN : COL_AMBER, FONT, TXT_TINY, COL_DIR, ty, rx ? "RX" : "TX");

        pax_draw_text(&fb, COL_WHITE, FONT, TXT_TINY, COL_TYPE, ty, d->valid ? diag_type_name(d->ptype) : "?");

        char rssi[8];
        if (rx && e->rssi_dbm != DIAG_RSSI_NONE) {
            snprintf(rssi, sizeof(rssi), "%d", e->rssi_dbm);
        } else {
            snprintf(rssi, sizeof(rssi), "--");
        }
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_TINY, COL_RSSI, ty, rssi);

        // SNR (RX only) one-decimal dB; sign kept for the sub-1 dB negatives the
        // detail screen drops. snr_db_x4 is SNR in quarter-dB.
        char snr[10];
        if (rx && e->rssi_dbm != DIAG_RSSI_NONE) {
            int x4 = e->snr_db_x4, neg = x4 < 0, a = neg ? -x4 : x4;
            snprintf(snr, sizeof(snr), "%s%d.%02d", neg ? "-" : "", a / 4, (a % 4) * 25);
        } else {
            snprintf(snr, sizeof(snr), "--");
        }
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_TINY, COL_SNR, ty, snr);

        char hop[6];
        if (d->valid) {
            snprintf(hop, sizeof(hop), "%u", d->hops);
        } else {
            snprintf(hop, sizeof(hop), "--");
        }
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_TINY, COL_HOPS, ty, hop);

        char detail[160];
        format_detail(e, d, detail, sizeof(detail));
        pax_draw_text(&fb, COL_PAGER_TEXT, FONT, TXT_TINY, COL_DETAIL, ty, detail);
    }

    int fy = h - LOG_FOOTER_H;
    pax_simple_rect(&fb, COL_HEADER, 0, fy, w, LOG_FOOTER_H);
    pax_simple_rect(&fb, COL_PAGER_ACCENT, 0, fy, w, 1);
    const char* log_hint = "WS: select  Enter: detail  H: hex/dissect  P: pause  E: export  C: clear  ";
    int         log_ty   = fy + (LOG_FOOTER_H - TXT_SMALL) / 2;
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 10, log_ty, log_hint);
    render_back_hint(10 + (int)pax_text_size(FONT, TXT_SMALL, log_hint).x, log_ty, ": back", TXT_SMALL);

    // Draw the export-result toast ("Saved N pkts -> ...") on top. Without this
    // the toast set by toolbox_log_export_sd() was invisible — only render_home
    // painted the shared overlay before.
    render_toast(w, h);
}
