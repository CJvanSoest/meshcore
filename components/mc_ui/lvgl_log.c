// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// VIEW_TOOLBOX_LOG.

#include <math.h>
#include <stdio.h>
#include <time.h>
#include "diag.h"
#include "diag_decode.h"
#include "history.h"
#include "lvgl.h"
#include "lvgl_internal.h"
#include "lvgl_port.h"
#include "render.h"
#include "render_internal.h"
#include "ui_state.h"

// ── VIEW_TOOLBOX_LOG ─────────────────────────────────────────────────────────
// Pixel-matched port of packet_log.c (the live packet log). The snapshot
// + format logic stays single-source in packet_log.c; this view only
// walks it (toolbox_log_snapshot / toolbox_log_snap_ri / toolbox_log_format_
// detail). The hex dump lines in the detail view use the UNSCII mono face so the
// bytes line up; everything else uses the same proportional Montserrat faces and
// absolute COL_* column x-positions as the PAX view.

#define LOG_HEADER_H 44
#define LOG_COLHDR_H 20
#define LOG_FOOTER_H 38
#define LOG_ROW_H    24

#define COL_TS     8
#define COL_DIR    62
#define COL_TYPE   96
#define COL_RSSI   166
#define COL_SNR    212
#define COL_HOPS   268
#define COL_DETAIL 312

static void log_header_lvgl(lv_obj_t* scr, int w) {
    add_rect(scr, 0, 0, w, LOG_HEADER_H, COL_HEADER);
    add_rect(scr, 0, LOG_HEADER_H - 1, w, 1, COL_PAGER_ACCENT);
    add_label(scr, 10, (LOG_HEADER_H - TXT_TAB) / 2, TXT_TAB, COL_WHITE, "Packet Log");

    char meta[48];
    snprintf(meta, sizeof(meta), "%s  %lu pkts", toolbox_log_dissect ? "DISSECT" : "HEX", (unsigned long)diag_total());
    int mx = w - text_w(meta, TXT_SMALL) - 12;
    if (toolbox_log_paused) {
        const char* p   = "PAUSED";
        int         pw  = text_w(p, TXT_SMALL);
        mx             -= pw + 12;
        add_label(scr, w - pw - 12, (LOG_HEADER_H - TXT_SMALL) / 2, TXT_SMALL, COL_RED, p);
    }
    add_label(scr, mx, (LOG_HEADER_H - TXT_SMALL) / 2, TXT_SMALL, COL_GRAY, meta);
}

static void log_col_headers_lvgl(lv_obj_t* scr, int w, int top) {
    int ly = top + (LOG_COLHDR_H - TXT_TINY) / 2;
    add_label(scr, COL_TS, ly, TXT_TINY, COL_WHITE, "Time");
    add_label(scr, COL_DIR, ly, TXT_TINY, COL_WHITE, "Dir");
    add_label(scr, COL_TYPE, ly, TXT_TINY, COL_WHITE, "Type");
    add_label(scr, COL_RSSI, ly, TXT_TINY, COL_WHITE, "RSSI");
    add_label(scr, COL_SNR, ly, TXT_TINY, COL_WHITE, "SNR");
    add_label(scr, COL_HOPS, ly, TXT_TINY, COL_WHITE, "Hops");
    add_label(scr, COL_DETAIL, ly, TXT_TINY, COL_WHITE, "Detail");
    add_rect(scr, 0, top + LOG_COLHDR_H - 1, w, 1, COL_HEADER);
}

// Full-screen breakdown of one captured frame. Mono only on the pubkey + raw
// byte hex lines so they line up; mirrors render_log_detail() in the PAX view.
static void render_log_detail_lvgl(lv_obj_t* scr, int w, int h, const diag_entry_t* e, const diag_decoded_t* d) {
    add_rect(scr, 0, 0, w, LOG_HEADER_H, COL_HEADER);
    add_rect(scr, 0, LOG_HEADER_H - 1, w, 1, COL_PAGER_ACCENT);
    add_label(scr, 10, (LOG_HEADER_H - TXT_TAB) / 2, TXT_TAB, COL_WHITE, "Packet Detail");

    int  x  = 12;
    int  y  = LOG_HEADER_H + 10;
    bool rx = (e->dir == DIAG_DIR_RX);
    char line[96];

    snprintf(line, sizeof(line), "%s   %s   %s", rx ? "RX" : "TX", d->valid ? diag_type_name(d->ptype) : "?",
             d->valid ? diag_route_name(d->route) : "");
    add_label(scr, x, y, TXT_BODY, rx ? COL_GREEN : COL_AMBER, line);
    y += TXT_BODY + 6;

    snprintf(line, sizeof(line), "time %lus   len %uB   hops %u", (unsigned long)(e->now_ms / 1000), e->full_len,
             d->hops);
    add_label(scr, x, y, TXT_SMALL, COL_GRAY, line);
    y += TXT_SMALL + 4;

    if (rx && e->rssi_dbm != DIAG_RSSI_NONE) {
        int sw = e->snr_db_x4 / 4, sf = (e->snr_db_x4 < 0 ? -e->snr_db_x4 : e->snr_db_x4) % 4 * 25;
        snprintf(line, sizeof(line), "RSSI %d dBm   SNR %d.%02d dB", e->rssi_dbm, sw, sf);
        add_label(scr, x, y, TXT_SMALL, COL_GRAY, line);
        y += TXT_SMALL + 4;
    }

    if (d->has_pubkey) {
        add_label(scr, x, y, TXT_SMALL, COL_AMBER, "Public key:");
        y += TXT_SMALL + 2;
        for (int half = 0; half < 2; half++) {
            add_hex_cols(scr, x + 8, y, COL_GREEN, &d->pubkey[half * 16], 16);
            y += TXT_SMALL + 2;
        }
        snprintf(line, sizeof(line), "role %s   name %s", diag_role_name(d->role), d->has_name ? d->name : "-");
        add_label(scr, x, y, TXT_SMALL, COL_PAGER_TEXT, line);
        y += TXT_SMALL + 6;
    } else if (d->has_hash) {
        snprintf(line, sizeof(line), "dst-hash %02X   src-hash %02X", d->dest_hash, d->src_hash);
        add_label(scr, x, y, TXT_SMALL, COL_PAGER_TEXT, line);
        y += TXT_SMALL + 6;
    }

    add_label(scr, x, y, TXT_SMALL, COL_AMBER, "Bytes (on-air):");
    y      += TXT_SMALL + 2;
    int fy  = h - LOG_FOOTER_H;
    for (int off = 0; off < e->raw_len && y < fy - TXT_SMALL; off += 16) {
        int n = e->raw_len - off;
        if (n > 16) n = 16;
        add_hex_cols(scr, x + 8, y, COL_PAGER_TEXT, &e->raw[off], n);
        y += TXT_SMALL + 2;
    }

    add_rect(scr, 0, fy, w, LOG_FOOTER_H, COL_HEADER);
    add_rect(scr, 0, fy, w, 1, COL_PAGER_ACCENT);
    add_back_hint(scr, 10, fy + (LOG_FOOTER_H - TXT_SMALL) / 2, ": back to list", TXT_SMALL);
}

void render_toolbox_log_lvgl(void) {
    int       w   = (int)lvgl_port_width();
    int       h   = (int)lvgl_port_height();
    lv_obj_t* scr = begin_screen(COL_BLACK);
    pt_reset();

    const diag_entry_t*   snap    = NULL;
    const diag_decoded_t* decoded = NULL;
    int                   count   = 0;
    int                   head    = 0;
    (void)head;  // mapping uses toolbox_log_snap_ri(), which reads the same head
    if (!toolbox_log_snapshot(&snap, &decoded, &count, &head)) {
        log_header_lvgl(scr, w);
        add_label(scr, 12, LOG_HEADER_H + 12, TXT_SMALL, COL_RED, "packet log unavailable (no memory)");
        return;
    }

    if (toolbox_log_cursor >= count) toolbox_log_cursor = count - 1;
    if (toolbox_log_cursor < 0) toolbox_log_cursor = 0;
    if (toolbox_log_detail && count > 0) {
        int ri = toolbox_log_snap_ri(toolbox_log_cursor);
        render_log_detail_lvgl(scr, w, h, &snap[ri], &decoded[ri]);
        return;
    }

    int rows_y0  = LOG_HEADER_H + 4 + LOG_COLHDR_H;
    int avail_h  = h - rows_y0 - LOG_FOOTER_H;
    int rows_vis = avail_h / LOG_ROW_H;
    if (rows_vis < 1) rows_vis = 1;

    if (toolbox_log_cursor < toolbox_log_scroll) toolbox_log_scroll = toolbox_log_cursor;
    if (toolbox_log_cursor >= toolbox_log_scroll + rows_vis) toolbox_log_scroll = toolbox_log_cursor - rows_vis + 1;
    int max_scroll = count - rows_vis;
    if (max_scroll < 0) max_scroll = 0;
    if (toolbox_log_scroll > max_scroll) toolbox_log_scroll = max_scroll;
    if (toolbox_log_scroll < 0) toolbox_log_scroll = 0;

    log_header_lvgl(scr, w);
    log_col_headers_lvgl(scr, w, LOG_HEADER_H + 4);

    if (count == 0) {
        add_label(scr, 12, rows_y0 + 8, TXT_SMALL, COL_GRAY, "Waiting for radio traffic...");
    }

    for (int i = 0; i < rows_vis; i++) {
        int si = toolbox_log_scroll + i;
        if (si >= count) break;
        int                   ri = toolbox_log_snap_ri(si);
        const diag_entry_t*   e  = &snap[ri];
        const diag_decoded_t* d  = &decoded[ri];
        int                   ry = rows_y0 + i * LOG_ROW_H;

        if (si == toolbox_log_cursor) {
            add_rect(scr, 0, ry, w, LOG_ROW_H, COL_PANEL);
            add_rect(scr, 0, ry, 3, LOG_ROW_H, COL_PAGER_ACCENT);
        } else if (i & 1) {
            add_rect(scr, 0, ry, w, LOG_ROW_H, COL_HEADER);
        }

        int ty = ry + (LOG_ROW_H - TXT_TINY) / 2;

        char ts[12];
        snprintf(ts, sizeof(ts), "%lus", (unsigned long)(e->now_ms / 1000));
        add_label(scr, COL_TS, ty, TXT_TINY, COL_GRAY, ts);

        bool rx = (e->dir == DIAG_DIR_RX);
        add_label(scr, COL_DIR, ty, TXT_TINY, rx ? COL_GREEN : COL_AMBER, rx ? "RX" : "TX");

        add_label(scr, COL_TYPE, ty, TXT_TINY, COL_WHITE, d->valid ? diag_type_name(d->ptype) : "?");

        char rssi[8];
        if (rx && e->rssi_dbm != DIAG_RSSI_NONE) {
            snprintf(rssi, sizeof(rssi), "%d", e->rssi_dbm);
        } else {
            snprintf(rssi, sizeof(rssi), "--");
        }
        add_label(scr, COL_RSSI, ty, TXT_TINY, COL_GRAY, rssi);

        char snr[10];
        if (rx && e->rssi_dbm != DIAG_RSSI_NONE) {
            int x4 = e->snr_db_x4, neg = x4 < 0, a = neg ? -x4 : x4;
            snprintf(snr, sizeof(snr), "%s%d.%02d", neg ? "-" : "", a / 4, (a % 4) * 25);
        } else {
            snprintf(snr, sizeof(snr), "--");
        }
        add_label(scr, COL_SNR, ty, TXT_TINY, COL_GRAY, snr);

        char hop[6];
        if (d->valid) {
            snprintf(hop, sizeof(hop), "%u", d->hops);
        } else {
            snprintf(hop, sizeof(hop), "--");
        }
        add_label(scr, COL_HOPS, ty, TXT_TINY, COL_GRAY, hop);

        char detail[160];
        toolbox_log_format_detail(e, d, detail, sizeof(detail));
        add_label(scr, COL_DETAIL, ty, TXT_TINY, COL_PAGER_TEXT, detail);
    }

    int fy = h - LOG_FOOTER_H;
    add_rect(scr, 0, fy, w, LOG_FOOTER_H, COL_HEADER);
    add_rect(scr, 0, fy, w, 1, COL_PAGER_ACCENT);
    const char* log_hint = "WS: select  Enter: detail  H: hex/dissect  P: pause  E: export  C: clear  ";
    int         log_ty   = fy + (LOG_FOOTER_H - TXT_SMALL) / 2;
    add_label(scr, 10, log_ty, TXT_SMALL, COL_HINT, log_hint);
    add_back_hint(scr, 10 + text_w(log_hint, TXT_SMALL), log_ty, ": back", TXT_SMALL);

    // Export-result toast ("Saved N pkts -> ...") on top, same helper the map
    // first-fix toast uses.
    status_toast_lvgl(scr, w, h);
}
