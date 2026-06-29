// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "lvgl_ui.h"
#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "esp_app_desc.h"
#include "lvgl.h"
#include "lvgl_port.h"
#include "render.h"  // COL_* palette + TXT_* sizes (shared with the PAX views)

// ── Shared LVGL helpers ──────────────────────────────────────────────────────

// COL_* are 0xAARRGGBB; LVGL wants RGB. Drop alpha.
static inline lv_color_t mc_col(uint32_t argb) {
    return lv_color_make((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
}

// Map the app's TXT_* point sizes onto the built-in Montserrat faces enabled in
// sdkconfig. TXT_TINY (13) has no 13 px face; 14 is the closest.
static const lv_font_t* mc_font(int sz) {
    switch (sz) {
        case TXT_TINY:
            return &lv_font_montserrat_14;
        case TXT_SMALL:
            return &lv_font_montserrat_16;
        case TXT_BODY:
            return &lv_font_montserrat_20;
        case TXT_TAB:
            return &lv_font_montserrat_22;
        case TXT_TITLE:
            return &lv_font_montserrat_24;
        default:
            return &lv_font_montserrat_16;
    }
}

// A flat label at absolute (x, y) — y is the top of the text, matching the PAX
// pax_draw_text origin. No padding, no scroll, no wrap.
static lv_obj_t* add_label(lv_obj_t* parent, int x, int y, int font_sz, uint32_t col, const char* text) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, mc_font(font_sz), 0);
    lv_obj_set_style_text_color(l, mc_col(col), 0);
    lv_obj_set_style_pad_all(l, 0, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

// A filled rectangle (PAX pax_simple_rect equivalent): used for header/footer
// strips and accent lines.
static lv_obj_t* add_rect(lv_obj_t* parent, int x, int y, int w, int h, uint32_t col) {
    lv_obj_t* r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_pos(r, x, y);
    lv_obj_set_size(r, w, h);
    lv_obj_set_style_bg_color(r, mc_col(col), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

// The red "X" back glyph + hint label (mirrors render_back_hint in render.c).
// Point arrays must outlive the render; the screen is rebuilt + reflushed every
// frame, so per-call static storage is safe (one back hint per LVGL view).
static void add_back_hint(lv_obj_t* parent, int x, int y, const char* label, int ts) {
    static lv_point_precise_t a[2];
    static lv_point_precise_t b[2];
    int                       xg = ts / 2 - 1;
    int                       cy = y + ts / 2;
    a[0]                         = (lv_point_precise_t){x, cy - xg};
    a[1]                         = (lv_point_precise_t){x + 2 * xg, cy + xg};
    b[0]                         = (lv_point_precise_t){x, cy + xg};
    b[1]                         = (lv_point_precise_t){x + 2 * xg, cy - xg};

    lv_obj_t* l1 = lv_line_create(parent);
    lv_line_set_points(l1, a, 2);
    lv_obj_set_style_line_color(l1, mc_col(COL_RED), 0);
    lv_obj_set_style_line_width(l1, 2, 0);
    lv_obj_set_pos(l1, 0, 0);

    lv_obj_t* l2 = lv_line_create(parent);
    lv_line_set_points(l2, b, 2);
    lv_obj_set_style_line_color(l2, mc_col(COL_RED), 0);
    lv_obj_set_style_line_width(l2, 2, 0);
    lv_obj_set_pos(l2, 0, 0);

    add_label(parent, x + 2 * xg + 4, y, ts, COL_HINT, label);
}

// Reset the persistent screen to the given solid background, ready for a rebuild.
static lv_obj_t* begin_screen(uint32_t bg_col) {
    lv_obj_t* scr = (lv_obj_t*)lvgl_port_screen();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, mc_col(bg_col), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    return scr;
}

// ── VIEW_ABOUT ───────────────────────────────────────────────────────────────
// Pixel-matched port of render_about.c.

#define ABOUT_HEADER_H 50
#define ABOUT_FOOTER_H 38

typedef struct {
    uint32_t    col;
    int         font_size;
    const char* text;
    int         space_after;
} about_line_t;

static void render_about_lvgl(void) {
    int       w   = (int)lvgl_port_width();
    int       h   = (int)lvgl_port_height();
    lv_obj_t* scr = begin_screen(COL_PAGER_BG);

    // Header strip + accent underline + title.
    add_rect(scr, 0, 0, w, ABOUT_HEADER_H, COL_PAGER_BG);
    add_rect(scr, 0, ABOUT_HEADER_H - 1, w, 1, COL_PAGER_ACCENT);
    add_label(scr, 12, (ABOUT_HEADER_H - TXT_TAB) / 2, TXT_TAB, COL_PAGER_TEXT, "About");

    const esp_app_desc_t* desc    = esp_app_get_description();
    const char*           ver     = (desc && desc->version[0]) ? desc->version : "?";
    const char*           built_d = (desc && desc->date[0]) ? desc->date : "?";
    const char*           built_t = (desc && desc->time[0]) ? desc->time : "";

    static char ver_line[80];
    snprintf(ver_line, sizeof(ver_line), "MeshCore  %s", ver);
    static char build_line[96];
    snprintf(build_line, sizeof(build_line), "Built %s  %s", built_d, built_t);

    about_line_t lines[] = {
        {COL_PAGER_ACCENT, TXT_TITLE, ver_line, 6},
        {COL_GRAY, TXT_SMALL, build_line, 6},
        {COL_AMBER, TXT_SMALL, "Community app -- not the official MeshCore app", 22},

        {COL_PAGER_TEXT, TXT_BODY, "Author", 4},
        {COL_GRAY, TXT_SMALL, "CJ van Soest (CJvS)", 16},

        {COL_PAGER_TEXT, TXT_BODY, "Built on", 4},
        {COL_GRAY, TXT_SMALL, "MeshCore  by  Ripple Radios", 2},
        {COL_GRAY, TXT_SMALL, "Tanmatsu  by  Nicolai Electronics", 16},

        {COL_PAGER_TEXT, TXT_BODY, "License", 4},
        {COL_GRAY, TXT_SMALL, "MIT (see LICENSE in the repo)", 16},

        {COL_PAGER_TEXT, TXT_BODY, "Source", 4},
        {COL_GRAY, TXT_SMALL, "github.com/CJvanSoest/meshcore", 6},

        {COL_PAGER_TEXT, TXT_BODY, "Issues / questions", 4},
        {COL_GRAY, TXT_SMALL, "github.com/CJvanSoest/meshcore/issues", 0},
    };
    const int n_lines = (int)(sizeof(lines) / sizeof(lines[0]));

    int x = 28;
    int y = ABOUT_HEADER_H + 24;
    for (int i = 0; i < n_lines; i++) {
        add_label(scr, x, y, lines[i].font_size, lines[i].col, lines[i].text);
        y += lines[i].font_size + 4 + lines[i].space_after;
    }

    // Footer strip + back hint.
    int fy = h - ABOUT_FOOTER_H;
    add_rect(scr, 0, fy, w, ABOUT_FOOTER_H, COL_HEADER);
    add_rect(scr, 0, fy, w, 1, COL_PAGER_ACCENT);
    add_back_hint(scr, 10, fy + (ABOUT_FOOTER_H - TXT_SMALL) / 2, ": home", TXT_SMALL);
}

// ── Dispatch ─────────────────────────────────────────────────────────────────

bool lvgl_view_active(app_view_t v) {
    switch (v) {
        case VIEW_ABOUT:
            return true;
        default:
            return false;
    }
}

void lvgl_view_render(app_view_t v) {
    switch (v) {
        case VIEW_ABOUT:
            render_about_lvgl();
            break;
        default:
            return;
    }
    lvgl_port_refresh_now();
}
