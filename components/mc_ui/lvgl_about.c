// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// VIEW_ABOUT.

#include <stdio.h>
#include <time.h>
#include "esp_app_desc.h"
#include "history.h"
#include "identity.h"
#include "lvgl.h"
#include "lvgl_internal.h"
#include "lvgl_port.h"
#include "render.h"
#include "ui_state.h"

// ── VIEW_ABOUT ───────────────────────────────────────────────────────────────
// Version, author, credits, licence and source.

#define ABOUT_HEADER_H 50
#define ABOUT_FOOTER_H 38

typedef struct {
    uint32_t    col;
    int         font_size;
    const char* text;
    int         space_after;
} about_line_t;

void render_about_lvgl(void) {
    int       w   = (int)lvgl_port_width();
    int       h   = (int)lvgl_port_height();
    lv_obj_t* scr = begin_screen(COL_PAGER_BG);

    // Header + footer are drawn AFTER the (scrollable) content below, so
    // scrolled-up lines are covered by the header strip instead of overlapping it.

    const esp_app_desc_t* desc    = esp_app_get_description();
    const char*           ver     = (desc && desc->version[0]) ? desc->version : "?";
    const char*           built_d = (desc && desc->date[0]) ? desc->date : "?";
    const char*           built_t = (desc && desc->time[0]) ? desc->time : "";

    static char ver_line[80];
    snprintf(ver_line, sizeof(ver_line), "MeshCore  %s", ver);
    static char build_line[96];
    snprintf(build_line, sizeof(build_line), "Built %s  %s", built_d, built_t);

    // Node public key (identity), split over two lines of 16 bytes each.
    static char pk_hi[40], pk_lo[40];
    for (int i = 0; i < 16; i++) snprintf(pk_hi + i * 2, 3, "%02x", node_pub_key[i]);
    for (int i = 0; i < 16; i++) snprintf(pk_lo + i * 2, 3, "%02x", node_pub_key[16 + i]);

    about_line_t lines[] = {
        {COL_PAGER_ACCENT, TXT_TITLE, ver_line, 6},
        {COL_GRAY, TXT_SMALL, build_line, 6},
        {COL_AMBER, TXT_SMALL, "Community app -- not the official MeshCore app", 22},

        {COL_PAGER_TEXT, TXT_BODY, "Author", 4},
        {COL_GRAY, TXT_SMALL, "CJ van Soest (CJvS)", 16},

        {COL_PAGER_TEXT, TXT_BODY, "Built on", 4},
        {COL_GRAY, TXT_SMALL, "MeshCore  by  Ripple Radios", 2},
        {COL_GRAY, TXT_SMALL, "Tanmatsu  by  Nicolai Electronics", 16},

        {COL_PAGER_TEXT, TXT_BODY, "Node public key", 4},
        {COL_GREEN, TXT_SMALL, pk_hi, 2},
        {COL_GREEN, TXT_SMALL, pk_lo, 16},

        {COL_PAGER_TEXT, TXT_BODY, "License", 4},
        {COL_GRAY, TXT_SMALL, "MIT (see LICENSE in the repo)", 16},

        {COL_PAGER_TEXT, TXT_BODY, "Source", 4},
        {COL_GRAY, TXT_SMALL, "github.com/CJvanSoest/meshcore", 6},

        {COL_PAGER_TEXT, TXT_BODY, "Issues / questions", 4},
        {COL_GRAY, TXT_SMALL, "github.com/CJvanSoest/meshcore/issues", 0},
    };
    const int n_lines = (int)(sizeof(lines) / sizeof(lines[0]));

    // Clamp the scroll to the content height, then draw the visible band.
    int total = 0;
    for (int i = 0; i < n_lines; i++) total += lines[i].font_size + 4 + lines[i].space_after;
    int top_y      = ABOUT_HEADER_H + 12;
    int bot_y      = h - ABOUT_FOOTER_H;
    int max_scroll = total - (bot_y - top_y);
    if (max_scroll < 0) max_scroll = 0;
    if (about_scroll > max_scroll) about_scroll = max_scroll;
    if (about_scroll < 0) about_scroll = 0;

    int x = 28;
    int y = top_y - about_scroll;
    for (int i = 0; i < n_lines; i++) {
        int lh = lines[i].font_size;
        if (y + lh > ABOUT_HEADER_H && y < bot_y)  // only lines within the content band
            add_label(scr, x, y, lines[i].font_size, lines[i].col, lines[i].text);
        y += lh + 4 + lines[i].space_after;
    }

    // Header strip + accent underline + title, over any scrolled-up content.
    add_rect(scr, 0, 0, w, ABOUT_HEADER_H, COL_PAGER_BG);
    add_rect(scr, 0, ABOUT_HEADER_H - 1, w, 1, COL_PAGER_ACCENT);
    add_label(scr, 12, (ABOUT_HEADER_H - TXT_TAB) / 2, TXT_TAB, COL_PAGER_TEXT, "About");

    // Footer strip + scroll/back hints.
    int fy = h - ABOUT_FOOTER_H;
    add_rect(scr, 0, fy, w, ABOUT_FOOTER_H, COL_HEADER);
    add_rect(scr, 0, fy, w, 1, COL_PAGER_ACCENT);
    int         ty   = fy + (ABOUT_FOOTER_H - TXT_SMALL) / 2;
    const char* hint = (max_scroll > 0) ? "WS: scroll   " : "";
    add_label(scr, 10, ty, TXT_SMALL, COL_HINT, hint);
    add_back_hint(scr, 10 + text_w(hint, TXT_SMALL), ty, ": home", TXT_SMALL);
}
