// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// VIEW_TOOLBOX_LOG — the live packet log (issue #3 a/b). Renders the diag ring
// (radio RX + TX frames) newest-first, one line per frame, in two modes:
//   HEX     — leading on-air bytes as a hex dump.
//   DISSECT — per-payload field breakdown (decoded in mc_proto/diag_decode).
// Pause freezes the displayed window while capture keeps running underneath.

#include "render.h"
#include "render_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "pax_gfx.h"
#include "pax_text.h"

#include "app_config.h"
#include "diag.h"
#include "diag_decode.h"
#include "ui_state.h"

#define LOG_HEADER_H 44
#define LOG_FOOTER_H 38
#define LOG_ROW_H    24

// Column x-positions (px). Kept narrow so the detail/hex column gets the rest.
#define COL_TS    8
#define COL_DIR   62
#define COL_TYPE  96
#define COL_RSSI  166
#define COL_DETAIL 210

// One PSRAM-resident snapshot of the ring, refreshed each live frame. Frozen
// (not refreshed) while paused so the user can read + scroll a stable window.
static diag_entry_t *s_snap       = NULL;
static int           s_snap_count = 0;

static void ensure_snap(void) {
    if (s_snap != NULL) return;
    size_t bytes = sizeof(diag_entry_t) * DIAG_LOG_SIZE;
    s_snap = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (s_snap == NULL) s_snap = malloc(bytes);
}

static void log_header(int w) {
    pax_simple_rect(&fb, COL_HEADER,    0, 0, w, LOG_HEADER_H);
    pax_simple_rect(&fb, COL_PAGER_ACCENT, 0, LOG_HEADER_H - 1, w, 1);
    int ty = (LOG_HEADER_H - TXT_TAB) / 2;
    pax_draw_text(&fb, COL_WHITE, FONT, TXT_TAB, 10, ty, "Packet Log");

    char meta[48];
    snprintf(meta, sizeof(meta), "%s  %lu pkts", toolbox_log_dissect ? "DISSECT" : "HEX",
             (unsigned long)diag_total());
    pax_vec2f sz = pax_text_size(FONT, TXT_SMALL, meta);
    int mx = w - (int)sz.x - 12;
    if (toolbox_log_paused) {
        const char *p = "PAUSED";
        pax_vec2f psz = pax_text_size(FONT, TXT_SMALL, p);
        mx -= (int)psz.x + 12;
        pax_draw_text(&fb, COL_RED, FONT, TXT_SMALL, w - (int)psz.x - 12,
                      (LOG_HEADER_H - TXT_SMALL) / 2, p);
    }
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, mx, (LOG_HEADER_H - TXT_SMALL) / 2, meta);
}

// Build the right-hand detail string for one entry into `out`.
static void format_detail(const diag_entry_t *e, const diag_decoded_t *d, char *out, size_t cap) {
    if (toolbox_log_dissect) {
        if (!d->valid) {
            snprintf(out, cap, "(unparsed) %uB", e->full_len);
            return;
        }
        int p = snprintf(out, cap, "%s h%u %uB ", diag_route_name(d->route), d->hops, e->full_len);
        if (p < 0 || (size_t)p >= cap) return;
        if (d->has_pubkey) {
            p += snprintf(out + p, cap - p, "key:%02X%02X%02X %s",
                          d->pubkey[0], d->pubkey[1], d->pubkey[2], diag_role_name(d->role));
            if (d->has_name && p > 0 && (size_t)p < cap) {
                snprintf(out + p, cap - p, " \"%s\"", d->name);
            }
        } else if (d->has_hash) {
            snprintf(out + p, cap - p, "dst:%02X src:%02X", d->dest_hash, d->src_hash);
        }
    } else {
        // Hex dump of the leading on-air bytes (as many as fit a row).
        int   p  = 0;
        int   nb = e->raw_len > 30 ? 30 : e->raw_len;
        for (int b = 0; b < nb && p < (int)cap - 4; b++) {
            p += snprintf(out + p, cap - p, "%02X ", e->raw[b]);
        }
        if (e->full_len > e->raw_len && p < (int)cap - 4) {
            snprintf(out + p, cap - p, "...");
        }
    }
}

void render_toolbox_log(void) {
    int w = (int)pax_buf_get_width(&fb);
    int h = (int)pax_buf_get_height(&fb);

    pax_background(&fb, COL_BLACK);
    ensure_snap();

    // Refresh the live window unless frozen. s_snap may be NULL if PSRAM alloc
    // failed; bail to a message rather than dereferencing it.
    if (s_snap == NULL) {
        log_header(w);
        pax_draw_text(&fb, COL_RED, FONT, TXT_SMALL, 12, LOG_HEADER_H + 12,
                      "packet log unavailable (no memory)");
        return;
    }
    if (!toolbox_log_paused) {
        s_snap_count = diag_snapshot(s_snap, DIAG_LOG_SIZE);
    }

    int rows_y0  = LOG_HEADER_H + 4;
    int avail_h  = h - rows_y0 - LOG_FOOTER_H;
    int rows_vis = avail_h / LOG_ROW_H;
    if (rows_vis < 1) rows_vis = 1;

    int max_scroll = s_snap_count - rows_vis;
    if (max_scroll < 0) max_scroll = 0;
    if (toolbox_log_scroll > max_scroll) toolbox_log_scroll = max_scroll;
    if (toolbox_log_scroll < 0) toolbox_log_scroll = 0;

    log_header(w);

    if (s_snap_count == 0) {
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 12, rows_y0 + 8,
                      "Waiting for radio traffic...");
    }

    for (int i = 0; i < rows_vis; i++) {
        int si = toolbox_log_scroll + i;
        if (si >= s_snap_count) break;
        const diag_entry_t *e = &s_snap[si];
        int ry = rows_y0 + i * LOG_ROW_H;

        if (i & 1) pax_simple_rect(&fb, COL_HEADER, 0, ry, w, LOG_ROW_H);

        diag_decoded_t d;
        diag_decode(e->raw, e->raw_len, &d);

        int ty = ry + (LOG_ROW_H - TXT_TINY) / 2;

        char ts[12];
        snprintf(ts, sizeof(ts), "%lus", (unsigned long)(e->now_ms / 1000));
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_TINY, COL_TS, ty, ts);

        bool rx = (e->dir == DIAG_DIR_RX);
        pax_draw_text(&fb, rx ? COL_GREEN : COL_AMBER, FONT, TXT_TINY, COL_DIR, ty,
                      rx ? "RX" : "TX");

        pax_draw_text(&fb, COL_WHITE, FONT, TXT_TINY, COL_TYPE, ty,
                      d.valid ? diag_type_name(d.ptype) : "?");

        char rssi[8];
        if (rx && e->rssi_dbm != DIAG_RSSI_NONE) {
            snprintf(rssi, sizeof(rssi), "%d", e->rssi_dbm);
        } else {
            snprintf(rssi, sizeof(rssi), "--");
        }
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_TINY, COL_RSSI, ty, rssi);

        char detail[160];
        format_detail(e, &d, detail, sizeof(detail));
        pax_draw_text(&fb, COL_PAGER_TEXT, FONT, TXT_TINY, COL_DETAIL, ty, detail);
    }

    int fy = h - LOG_FOOTER_H;
    pax_simple_rect(&fb, COL_HEADER,       0, fy, w, LOG_FOOTER_H);
    pax_simple_rect(&fb, COL_PAGER_ACCENT, 0, fy, w, 1);
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 10,
                  fy + (LOG_FOOTER_H - TXT_SMALL) / 2,
                  "WS: scroll  H: hex/dissect  P: pause  C: clear  ESC: back");
}
