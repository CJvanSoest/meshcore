// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// VIEW_TOOLBOX_COVERAGE — repeater coverage test (Toolbox iteration 2, #3).
// Lists discovered repeaters; Enter pings the selected one 3x (handled in
// input.c -> coverage_ping_start), colour-coding the result and logging every
// attempt to SD. Display only; the ping controller + results live in
// mc_rx / mc_domain.

#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "coverage.h"
#include "pax_gfx.h"
#include "pax_text.h"
#include "render.h"
#include "render_internal.h"
#include "ui_state.h"

#define COV_HEADER_H 50
#define COV_FOOTER_H 38
#define COV_ROW_H    44

// Shared snapshot buffer; input.c re-collects independently for the Enter path.
static coverage_repeater_t s_reps[COVERAGE_MAX_RESULTS];

static void cov_header(int w) {
    pax_simple_rect(&fb, COL_PAGER_BG, 0, 0, w, COV_HEADER_H);
    pax_simple_rect(&fb, COL_PAGER_ACCENT, 0, COV_HEADER_H - 1, w, 1);
    int ty = (COV_HEADER_H - TXT_TAB) / 2;
    pax_draw_text(&fb, COL_PAGER_TEXT, FONT, TXT_TAB, 12, ty, "Coverage Test");
    if (coverage_busy()) {
        const char* t  = "testing...";
        pax_vec2f   sz = pax_text_size(FONT, TXT_SMALL, t);
        pax_draw_text(&fb, COL_AMBER, FONT, TXT_SMALL, w - (int)sz.x - 12, (COV_HEADER_H - TXT_SMALL) / 2, t);
    }
}

// Map a result to a colour + short status string.
static pax_col_t status_render(const coverage_result_t* r, char* buf, size_t cap) {
    if (r == NULL) {
        snprintf(buf, cap, "-");
        return COL_GRAY;
    }
    snprintf(buf, cap, "%u/%u", r->acks, r->attempts);
    switch (r->status) {
        case COVERAGE_OK:
            return COL_GREEN;
        case COVERAGE_PARTIAL:
            return COL_AMBER;
        case COVERAGE_FAIL:
            return COL_RED;
        case COVERAGE_TESTING:
            return COL_AMBER;
        default:
            return COL_GRAY;
    }
}

void render_toolbox_coverage(void) {
    int w = (int)pax_buf_get_width(&fb);
    int h = (int)pax_buf_get_height(&fb);

    pax_background(&fb, COL_PAGER_BG);
    cov_header(w);

    int count = coverage_collect_repeaters(s_reps, COVERAGE_MAX_RESULTS);

    if (toolbox_coverage_cursor < 0) toolbox_coverage_cursor = 0;
    if (toolbox_coverage_cursor >= count) toolbox_coverage_cursor = count > 0 ? count - 1 : 0;

    int rows_y0  = COV_HEADER_H + 8;
    int avail_h  = h - rows_y0 - COV_FOOTER_H;
    int rows_vis = avail_h / COV_ROW_H;
    if (rows_vis < 1) rows_vis = 1;

    int scroll = 0;
    if (toolbox_coverage_cursor >= rows_vis) scroll = toolbox_coverage_cursor - rows_vis + 1;

    if (count == 0) {
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 16, rows_y0 + 8, "No repeaters discovered yet.");
    }

    for (int row = 0; row < rows_vis; row++) {
        int i = scroll + row;
        if (i >= count) break;
        int  ry      = rows_y0 + row * COV_ROW_H;
        bool focused = (i == toolbox_coverage_cursor);

        if (focused) {
            pax_simple_rect(&fb, COL_PANEL, 8, ry, w - 16, COV_ROW_H - 4);
            pax_simple_rect(&fb, COL_ACCENT, 8, ry, 3, COV_ROW_H - 4);
        }

        const char* name = s_reps[i].name[0] ? s_reps[i].name : "(unnamed)";
        pax_draw_text(&fb, COL_PAGER_TEXT, FONT, TXT_BODY, 20, ry + 6, name);

        char keyhex[16];
        snprintf(keyhex, sizeof(keyhex), "%02X%02X%02X", s_reps[i].pub[0], s_reps[i].pub[1], s_reps[i].pub[2]);
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 20, ry + 6 + TXT_BODY + 2, keyhex);

        coverage_result_t res;
        bool              have = coverage_lookup(s_reps[i].pub, &res);
        char              st[12];
        pax_col_t         col = status_render(have ? &res : NULL, st, sizeof(st));
        pax_vec2f         sz  = pax_text_size(FONT, TXT_BODY, st);
        pax_draw_text(&fb, col, FONT, TXT_BODY, w - (int)sz.x - 24, ry + (COV_ROW_H - TXT_BODY) / 2, st);
    }

    int fy = h - COV_FOOTER_H;
    pax_simple_rect(&fb, COL_HEADER, 0, fy, w, COV_FOOTER_H);
    pax_simple_rect(&fb, COL_PAGER_ACCENT, 0, fy, w, 1);
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 10, fy + (COV_FOOTER_H - TXT_SMALL) / 2,
                  "WS: nav   Enter: ping 3x   R: new session   ESC: back");
}
