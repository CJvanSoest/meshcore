// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// VIEW_TOOLBOX_COVERAGE.

#include "lvgl_internal.h"
#include "lvgl_port.h"
#include "lvgl_ui.h"
#include "map.h"
#include "mc_fonts.h"  // extended Montserrat faces (umlauts, accents, °, €, …)
#include "nodes.h"
#include "nvs_flash.h"
#include "qrcodegen.h"
#include "radio.h"
#include "region_limits.h"
#include "render.h"  // COL_* palette + TXT_* sizes
#include "render_internal.h"
#include "settings_nvs.h"
#include "special_table.h"  // special_font_covers (draw umlauts instead of '?')
#include "ui_state.h"
#include "wifi_connection.h"

// ── VIEW_TOOLBOX_COVERAGE ────────────────────────────────────────────────────
// Port of render_toolbox_coverage.c.

#define COV_HEADER_H 50
#define COV_FOOTER_H 38
#define COV_ROW_H    52

static coverage_repeater_t s_cov_reps[COVERAGE_MAX_RESULTS];

static uint32_t cov_status(const coverage_result_t* r, char* buf, size_t cap) {
    if (r == NULL) {
        snprintf(buf, cap, "-");
        return COL_GRAY;
    }
    snprintf(buf, cap, "%u/%u", r->acks, r->attempts);
    switch (r->status) {
        case COVERAGE_OK:
            return COL_GREEN;
        case COVERAGE_PARTIAL:
        case COVERAGE_TESTING:
            return COL_AMBER;
        case COVERAGE_FAIL:
            return COL_RED;
        default:
            return COL_GRAY;
    }
}

void render_toolbox_coverage_lvgl(void) {
    int       w   = (int)lvgl_port_width();
    int       h   = (int)lvgl_port_height();
    lv_obj_t* scr = begin_screen(COL_PAGER_BG);
    pt_reset();

    add_rect(scr, 0, 0, w, COV_HEADER_H, COL_PAGER_BG);
    add_rect(scr, 0, COV_HEADER_H - 1, w, 1, COL_PAGER_ACCENT);
    add_label(scr, 12, (COV_HEADER_H - TXT_TAB) / 2, TXT_TAB, COL_PAGER_TEXT, "Coverage Test");
    if (coverage_busy()) {
        const char* t = "testing...";
        add_label(scr, w - text_w(t, TXT_SMALL) - 12, (COV_HEADER_H - TXT_SMALL) / 2, TXT_SMALL, COL_AMBER, t);
    }

    bool    rv    = gps_live_valid || gps_position_valid;
    int32_t rlat  = gps_live_valid ? gps_live_lat_e6 : gps_lat_e6;
    int32_t rlon  = gps_live_valid ? gps_live_lon_e6 : gps_lon_e6;
    int     count = coverage_collect_repeaters(s_cov_reps, COVERAGE_MAX_RESULTS, rlat, rlon, rv, COVERAGE_RADIUS_M);

    if (toolbox_coverage_cursor < 0) toolbox_coverage_cursor = 0;
    if (toolbox_coverage_cursor >= count) toolbox_coverage_cursor = count > 0 ? count - 1 : 0;

    int rows_y0  = COV_HEADER_H + 8;
    int avail_h  = h - rows_y0 - COV_FOOTER_H;
    int rows_vis = avail_h / COV_ROW_H;
    if (rows_vis < 1) rows_vis = 1;

    int scroll = 0;
    if (toolbox_coverage_cursor >= rows_vis) scroll = toolbox_coverage_cursor - rows_vis + 1;

    if (count == 0) {
        add_label(scr, 16, rows_y0 + 8, TXT_SMALL, COL_GRAY, "No repeaters discovered yet.");
    }

    for (int row = 0; row < rows_vis; row++) {
        int i = scroll + row;
        if (i >= count) break;
        int  ry  = rows_y0 + row * COV_ROW_H;
        bool foc = (i == toolbox_coverage_cursor);
        if (foc) {
            add_rect(scr, 8, ry, w - 16, COV_ROW_H - 4, COL_PANEL);
            add_rect(scr, 8, ry, 3, COV_ROW_H - 4, COL_ACCENT);
        }
        const char* name = s_cov_reps[i].name[0] ? s_cov_reps[i].name : "(unnamed)";
        add_label(scr, 20, ry + 6, TXT_BODY, COL_PAGER_TEXT, name);

        char keyhex[16];
        snprintf(keyhex, sizeof(keyhex), "%02X%02X%02X", s_cov_reps[i].pub[0], s_cov_reps[i].pub[1],
                 s_cov_reps[i].pub[2]);
        add_label(scr, 20, ry + 6 + TXT_BODY + 2, TXT_SMALL, COL_GRAY, keyhex);

        coverage_result_t res;
        bool              have = coverage_lookup(s_cov_reps[i].pub, &res);
        char              st[12];
        uint32_t          col = cov_status(have ? &res : NULL, st, sizeof(st));
        add_label(scr, w - text_w(st, TXT_BODY) - 24, ry + (COV_ROW_H - TXT_BODY) / 2, TXT_BODY, col, st);
    }

    int fy = h - COV_FOOTER_H;
    add_rect(scr, 0, fy, w, COV_FOOTER_H, COL_HEADER);
    add_rect(scr, 0, fy, w, 1, COL_PAGER_ACCENT);
    const char* hint = "WS: nav   Enter: ping 3x   R: new session   ";
    int         ty   = fy + (COV_FOOTER_H - TXT_SMALL) / 2;
    add_label(scr, 10, ty, TXT_SMALL, COL_HINT, hint);
    add_back_hint(scr, 10 + text_w(hint, TXT_SMALL), ty, ": back", TXT_SMALL);
}
