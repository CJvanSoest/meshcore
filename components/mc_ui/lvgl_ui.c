// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "lvgl_ui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "bsp/power.h"
#include "channels.h"
#include "contacts.h"
#include "coverage.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gps_task.h"
#include "identity.h"
#include "lvgl.h"
#include "lvgl_port.h"
#include "nodes.h"
#include "radio.h"
#include "render.h"  // COL_* palette + TXT_* sizes (shared with the PAX views)
#include "render_internal.h"
#include "settings_nvs.h"
#include "ui_state.h"

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

// Exact rendered width of `text` in the given size's face — the LVGL analogue
// of pax_text_size().x, used for centring and right-alignment.
static int text_w(const char* text, int font_sz) {
    lv_point_t sz;
    lv_text_get_size(&sz, text, mc_font(font_sz), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return (int)sz.x;
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

// ── Vector-glyph primitives (for the Home tile icons) ────────────────────────
// lv_line keeps a pointer to the caller's point array, so the points must live
// until the frame is flushed. A per-frame pool reset at the top of each render
// supplies that storage; the screen is flushed synchronously before the next
// render reuses the pool.
#define PT_POOL 768
static lv_point_precise_t s_pt[PT_POOL];
static int                s_pt_n;

static void pt_reset(void) {
    s_pt_n = 0;
}

static void add_line(lv_obj_t* p, int x1, int y1, int x2, int y2, int w, uint32_t col) {
    if (s_pt_n + 2 > PT_POOL) {
        return;
    }
    lv_point_precise_t* pts  = &s_pt[s_pt_n];
    s_pt_n                  += 2;
    pts[0].x                 = x1;
    pts[0].y                 = y1;
    pts[1].x                 = x2;
    pts[1].y                 = y2;
    lv_obj_t* l              = lv_line_create(p);
    lv_line_set_points(l, pts, 2);
    lv_obj_set_style_line_color(l, mc_col(col), 0);
    lv_obj_set_style_line_width(l, w, 0);
    lv_obj_set_pos(l, 0, 0);
}

// Circle centred at (cx, cy). fill < 0 -> no fill; border < 0 -> no border.
// 64-bit so a 0xFFxxxxxx colour stays positive against the -1 sentinel.
static void add_circle(lv_obj_t* p, int cx, int cy, int r, int64_t fill, int64_t border, int bw) {
    lv_obj_t* o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, 2 * r, 2 * r);
    lv_obj_set_pos(o, cx - r, cy - r);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    if (fill >= 0) {
        lv_obj_set_style_bg_color(o, mc_col((uint32_t)fill), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    }
    if (border >= 0) {
        lv_obj_set_style_border_color(o, mc_col((uint32_t)border), 0);
        lv_obj_set_style_border_width(o, bw, 0);
        lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
    }
}

// Static arc stroke from start_deg to end_deg (LVGL angles: 0 deg = 3 o'clock,
// increasing clockwise; wraps through 0 if start > end).
static void add_arc(lv_obj_t* p, int cx, int cy, int r, int start_deg, int end_deg, int w, uint32_t col) {
    lv_obj_t* a = lv_arc_create(p);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(a, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(a, 2 * r, 2 * r);
    lv_obj_set_pos(a, cx - r, cy - r);
    lv_arc_set_rotation(a, 0);
    lv_arc_set_bg_angles(a, start_deg, end_deg);
    lv_arc_set_angles(a, start_deg, end_deg);
    lv_obj_set_style_arc_color(a, mc_col(col), LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, w, LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, mc_col(col), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(a, w, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, LV_PART_KNOB);
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

// ── VIEW_HOME ────────────────────────────────────────────────────────────────
// Tile-grid landing screen, pixel-matched to render_home.c. The tile metadata
// (labels, order, targets, unread badges) mirrors home_tiles[] there; keep the
// two in step. Icons are widget-built approximations of the PAX vector glyphs
// (LVGL's bitmap fonts can't scale to the ~60 px glyph sizes the PAX path uses).

#define HOME_TILE_COLS  3
#define HOME_TILE_ROWS  3
#define HOME_TILE_COUNT (HOME_TILE_COLS * HOME_TILE_ROWS)
#define HOME_H_MARGIN   30
#define HOME_V_MARGIN   20
#define HOME_HEADER_H   50
#define HOME_FOOTER_H   60

typedef enum {
    IC_NODES,
    IC_DM,
    IC_CHANNEL,
    IC_MAP,
    IC_ANTENNA,
    IC_SETTINGS,
    IC_ABOUT,
    IC_QR,
    IC_EXIT,
} icon_id_t;

static void home_icon(lv_obj_t* p, icon_id_t id, int cx, int cy, int sz, uint32_t col) {
    switch (id) {
        case IC_NODES: {
            int r = sz / 10, off = sz / 3;
            int ax = cx - off, ay = cy - off / 2, bx = cx + off, by = cy - off / 2, ccx = cx, ccy = cy + off;
            add_line(p, ax, ay, bx, by, 2, col);
            add_line(p, ax, ay, ccx, ccy, 2, col);
            add_line(p, bx, by, ccx, ccy, 2, col);
            add_circle(p, ax, ay, r, col, -1, 0);
            add_circle(p, bx, by, r, col, -1, 0);
            add_circle(p, ccx, ccy, r, col, -1, 0);
            break;
        }
        case IC_DM: {
            int w = sz, h = sz * 7 / 10;
            int x = cx - w / 2, y = cy - h / 2;
            add_rect(p, x, y, w, h, col);
            add_line(p, x + w / 5, y + h, x + w / 4, y + h + h / 3, 2, col);
            add_line(p, x + w / 4, y + h + h / 3, x + w / 3, y + h, 2, col);
            break;
        }
        case IC_CHANNEL: {
            int q = sz / 6, L = sz * 7 / 20;
            add_line(p, cx - q, cy - L, cx - q, cy + L, 2, col);
            add_line(p, cx + q, cy - L, cx + q, cy + L, 2, col);
            add_line(p, cx - L, cy - q, cx + L, cy - q, 2, col);
            add_line(p, cx - L, cy + q, cx + L, cy + q, 2, col);
            break;
        }
        case IC_MAP:
            add_circle(p, cx, cy, sz / 2, -1, col, 2);
            add_line(p, cx - sz / 2, cy, cx + sz / 2, cy, 2, col);
            add_circle(p, cx, cy, sz / 4, -1, col, 2);
            break;
        case IC_ANTENNA: {
            int half = sz / 2, top_y = cy - half + half / 4, base_y = cy + half - half / 8;
            int base_dx = half * 3 / 5, circle_r = sz / 10;
            add_circle(p, cx, top_y, circle_r, -1, col, 2);
            int mast_top_y = top_y + circle_r;
            add_line(p, cx, mast_top_y, cx - base_dx, base_y, 2, col);
            add_line(p, cx, mast_top_y, cx + base_dx, base_y, 2, col);
            int cross_y = mast_top_y + (base_y - mast_top_y) * 6 / 10, cross_dx = base_dx * 6 / 10;
            add_line(p, cx - cross_dx, cross_y, cx + cross_dx, cross_y, 2, col);
            int r1 = (int)((half - circle_r) * 0.55f), r2 = (int)((half - circle_r) * 0.85f);
            add_arc(p, cx, top_y, r1, 135, 225, 2, col);
            add_arc(p, cx, top_y, r2, 135, 225, 2, col);
            add_arc(p, cx, top_y, r1, 315, 45, 2, col);
            add_arc(p, cx, top_y, r2, 315, 45, 2, col);
            break;
        }
        case IC_SETTINGS: {
            int ro = sz / 3, ri = sz / 4, rm = (ro + ri) / 2, bw = ro - ri;
            if (bw < 2) bw = 2;
            add_circle(p, cx, cy, rm, -1, col, bw);
            for (int a = 0; a < 8; a++) {
                float t  = (float)a * 3.14159f / 4.0f;
                int   ox = cx + (int)((sz / 2.2f) * cosf(t));
                int   oy = cy + (int)((sz / 2.2f) * sinf(t));
                add_circle(p, ox, oy, sz / 12, col, -1, 0);
            }
            break;
        }
        case IC_ABOUT:
            add_circle(p, cx, cy, sz / 2, -1, col, 2);
            add_circle(p, cx, cy - sz / 5, sz / 14, col, -1, 0);
            add_line(p, cx, cy - sz / 12, cx, cy + sz / 4, 3, col);
            break;
        case IC_QR: {
            int b = sz / 8, g = sz / 4;
            for (int row = 0; row < 3; row++) {
                for (int c = 0; c < 3; c++) {
                    if ((row + c) % 2 == 0) {
                        add_rect(p, cx - sz / 2 + c * g + b, cy - sz / 2 + row * g + b, b, b, col);
                    }
                }
            }
            break;
        }
        case IC_EXIT: {
            int r = (int)(sz / 2.2f);
            add_arc(p, cx, cy, r, 300, 240, 2, col);
            add_rect(p, cx - sz / 22, cy - r - sz / 8, sz / 11, sz / 2, col);
            break;
        }
    }
}

typedef struct {
    const char* label;
    icon_id_t   icon;
    int         unread;  // 0 none, 1 = DM, 2 = channel
} home_tile_meta_t;

static const home_tile_meta_t home_meta[HOME_TILE_COUNT] = {
    {"Nodes", IC_NODES, 0}, {"DM", IC_DM, 1},          {"Channel", IC_CHANNEL, 2},
    {"Map", IC_MAP, 0},     {"Advert", IC_ANTENNA, 0}, {"Settings", IC_SETTINGS, 0},
    {"About", IC_ABOUT, 0}, {"QR", IC_QR, 0},          {"Exit", IC_EXIT, 0},
};

static void home_status_right(lv_obj_t* scr, int x_right, int ty, int font) {
    int x = x_right;

    bsp_power_battery_information_t bat = {0};
    if (bsp_power_get_battery_information(&bat) == ESP_OK && bat.battery_available) {
        int pct = (int)bat.remaining_percentage;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        char buf[16];
        snprintf(buf, sizeof(buf), "%d%%%s", pct, bat.battery_charging ? "+" : "");
        uint32_t col  = pct <= 20 ? COL_RED : (pct <= 50 ? COL_AMBER : COL_GREEN);
        x            -= text_w(buf, font);
        add_label(scr, x, ty, font, col, buf);
        x -= 14;
    }
    if (dc_budget_ms > 0 && dc_budget_ms < 3600000u) {
        unsigned pct_x10 = (unsigned)(((uint64_t)dc_used_ms * 1000u) / dc_budget_ms);
        char     buf[16];
        snprintf(buf, sizeof(buf), "TX:%u.%u%%", pct_x10 / 10u, pct_x10 % 10u);
        uint32_t col  = dc_last_tx_blocked ? COL_RED : (pct_x10 >= 800) ? COL_AMBER : COL_PAGER_TEXT;
        x            -= text_w(buf, font);
        add_label(scr, x, ty, font, col, buf);
        x -= 14;
    }
    if (lora_rx_ok) {
        int cnt = 0;
        if (rx_mutex && xSemaphoreTake(rx_mutex, 0) == pdTRUE) {
            cnt = rx_count;
            xSemaphoreGive(rx_mutex);
        }
        char buf[16];
        snprintf(buf, sizeof(buf), "RX:%d", cnt);
        x -= text_w(buf, font);
        add_label(scr, x, ty, font, COL_GREEN, buf);
    }
}

static void render_home_lvgl(void) {
    int       w   = (int)lvgl_port_width();
    int       h   = (int)lvgl_port_height();
    lv_obj_t* scr = begin_screen(COL_PAGER_BG);
    pt_reset();

    // Header: owner name (left) + RX/TX/battery (right).
    add_rect(scr, 0, 0, w, HOME_HEADER_H, COL_PAGER_BG);
    add_rect(scr, 0, HOME_HEADER_H - 1, w, 1, COL_PAGER_ACCENT);
    int         ty   = (HOME_HEADER_H - TXT_BODY) / 2;
    const char* name = lora_advert_name[0] ? lora_advert_name : (owner_name[0] ? owner_name : "(no name)");
    add_label(scr, 12, ty, TXT_BODY, COL_PAGER_TEXT, name);
    home_status_right(scr, w - 12, ty, TXT_BODY);

    // Tile grid.
    int area_y0 = HOME_HEADER_H + HOME_V_MARGIN;
    int area_h  = h - area_y0 - HOME_V_MARGIN - HOME_FOOTER_H;
    int area_w  = w - HOME_H_MARGIN * 2;
    int tile_w  = (area_w - HOME_H_MARGIN * (HOME_TILE_COLS - 1)) / HOME_TILE_COLS;
    int tile_h  = (area_h - HOME_V_MARGIN * (HOME_TILE_ROWS - 1)) / HOME_TILE_ROWS;

    if (home_cursor < 0) home_cursor = 0;
    if (home_cursor >= HOME_TILE_COUNT) home_cursor = HOME_TILE_COUNT - 1;

    for (int i = 0; i < HOME_TILE_COUNT; i++) {
        int  col_i = i % HOME_TILE_COLS;
        int  row_i = i / HOME_TILE_COLS;
        int  tx    = HOME_H_MARGIN + col_i * (tile_w + HOME_H_MARGIN);
        int  tyt   = area_y0 + row_i * (tile_h + HOME_V_MARGIN);
        bool foc   = (i == home_cursor);

        uint32_t bg = foc ? COL_PAGER_ACCENT : COL_PAGER_TILE;
        uint32_t fg = foc ? COL_HEADER : COL_PAGER_TEXT;
        add_rect(scr, tx, tyt, tile_w, tile_h, bg);
        if (foc) {
            add_rect(scr, tx + 2, tyt + 2, tile_w - 4, 2, COL_PAGER_BG);
            add_rect(scr, tx + 2, tyt + tile_h - 4, tile_w - 4, 2, COL_PAGER_BG);
            add_rect(scr, tx + 2, tyt + 2, 2, tile_h - 4, COL_PAGER_BG);
            add_rect(scr, tx + tile_w - 4, tyt + 2, 2, tile_h - 4, COL_PAGER_BG);
        }

        int icon_sz = tile_w / 2;
        if (icon_sz > tile_h * 2 / 5) icon_sz = tile_h * 2 / 5;
        home_icon(scr, home_meta[i].icon, tx + tile_w / 2, tyt + tile_h * 2 / 5, icon_sz, fg);

        // Label centred in the lower third.
        const char* lbl = home_meta[i].label;
        int         lw  = text_w(lbl, TXT_BODY);
        add_label(scr, tx + (tile_w - lw) / 2, tyt + tile_h * 2 / 3, TXT_BODY, fg, lbl);

        // Unread badge (DM, Channel).
        int count = home_meta[i].unread == 1   ? contact_unread_total()
                    : home_meta[i].unread == 2 ? channel_unread_total()
                                               : 0;
        if (count > 0) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d", count > 99 ? 99 : count);
            int bw = text_w(buf, TXT_SMALL) + 14;
            int bh = TXT_SMALL + 8;
            int bx = tx + tile_w - bw - 10;
            int by = tyt + 10;
            add_rect(scr, bx, by, bw, bh, COL_RED);
            add_label(scr, bx + (bw - text_w(buf, TXT_SMALL)) / 2, by + 4, TXT_SMALL, COL_PAGER_BG, buf);
        }
    }

    // Footer: two hint lines.
    int fy = h - HOME_FOOTER_H;
    add_rect(scr, 0, fy, w, HOME_FOOTER_H, COL_HEADER);
    add_rect(scr, 0, fy, w, 1, COL_PAGER_ACCENT);
    int line_h     = TXT_SMALL + 4;
    int hint_y_top = fy + (HOME_FOOTER_H - 2 * line_h) / 2;
    int hint_y     = hint_y_top + line_h;

    const char* pre = "Press ";
    add_label(scr, 10, hint_y_top, TXT_SMALL, COL_HINT, pre);
    int icon_sz = TXT_SMALL - 4;
    int icon_x  = 10 + text_w(pre, TXT_SMALL);
    int icon_y  = hint_y_top + (TXT_SMALL - icon_sz) / 2;
    add_rect(scr, icon_x, icon_y, icon_sz, icon_sz, COL_YELLOW);
    add_label(scr, icon_x + icon_sz + 4, hint_y_top, TXT_SMALL, COL_HINT, " to blank / wake display");

    const char* nav_hint = "WSAD: nav   Enter: open   Tab: tabs   ";
    add_label(scr, 10, hint_y, TXT_SMALL, COL_HINT, nav_hint);
    int x_x = 10 + text_w(nav_hint, TXT_SMALL);
    add_back_hint(scr, x_x, hint_y, ": home   ESC: exit", TXT_SMALL);
}

// ── VIEW_TOOLBOX ─────────────────────────────────────────────────────────────
// Port of render_toolbox.c. Tile metadata mirrors toolbox_tiles[] there.

#define TB_HEADER_H 50
#define TB_FOOTER_H 38
#define TB_ROW_H    64

typedef struct {
    const char* label;
    const char* desc;
    bool        enabled;
} tb_meta_t;

static const tb_meta_t tb_meta[] = {
    {"Packet Log", "Live RX/TX frames, hex dump + dissector", true},
    {"Coverage Test", "Ping repeaters, log reachability to SD", true},
};
#define TB_COUNT ((int)(sizeof(tb_meta) / sizeof(tb_meta[0])))

static void render_toolbox_lvgl(void) {
    int       w   = (int)lvgl_port_width();
    int       h   = (int)lvgl_port_height();
    lv_obj_t* scr = begin_screen(COL_PAGER_BG);
    pt_reset();

    add_rect(scr, 0, 0, w, TB_HEADER_H, COL_PAGER_BG);
    add_rect(scr, 0, TB_HEADER_H - 1, w, 1, COL_PAGER_ACCENT);
    add_label(scr, 12, (TB_HEADER_H - TXT_TAB) / 2, TXT_TAB, COL_PAGER_TEXT, "Toolbox");

    if (toolbox_cursor < 0) toolbox_cursor = 0;
    if (toolbox_cursor >= TB_COUNT) toolbox_cursor = TB_COUNT - 1;

    int x  = 20;
    int rw = w - 40;
    int y  = TB_HEADER_H + 20;
    for (int i = 0; i < TB_COUNT; i++) {
        bool foc = (i == toolbox_cursor);
        add_rect(scr, x, y, rw, TB_ROW_H, foc ? COL_PAGER_ACCENT : COL_PAGER_TILE);
        uint32_t title_col = tb_meta[i].enabled ? (foc ? COL_HEADER : COL_PAGER_TEXT) : COL_GRAY;
        uint32_t desc_col  = foc ? COL_HEADER : COL_GRAY;
        add_label(scr, x + 16, y + 12, TXT_BODY, title_col, tb_meta[i].label);
        add_label(scr, x + 16, y + 12 + TXT_BODY + 4, TXT_BODY, desc_col, tb_meta[i].desc);
        if (!tb_meta[i].enabled) {
            const char* tag = "soon";
            add_label(scr, x + rw - text_w(tag, TXT_SMALL) - 16, y + (TB_ROW_H - TXT_SMALL) / 2, TXT_SMALL, COL_AMBER,
                      tag);
        }
        y += TB_ROW_H + 14;
    }

    int fy = h - TB_FOOTER_H;
    add_rect(scr, 0, fy, w, TB_FOOTER_H, COL_HEADER);
    add_rect(scr, 0, fy, w, 1, COL_PAGER_ACCENT);
    const char* hint = "WS: nav   Enter: open   ";
    int         ty   = fy + (TB_FOOTER_H - TXT_SMALL) / 2;
    add_label(scr, 10, ty, TXT_SMALL, COL_HINT, hint);
    add_back_hint(scr, 10 + text_w(hint, TXT_SMALL), ty, ": settings", TXT_SMALL);
}

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

static void render_toolbox_coverage_lvgl(void) {
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

// ── Shared tab-bar header (classic views: Settings / Nodes / DM / Channel) ───
// Pixel-matched port of render_tab_bar() in render.c. Left: view name + inline
// DM / channel unread badges; right: RX | TX% | battery (reuses
// home_status_right, identical layout).

static void tab_bar_lvgl(lv_obj_t* scr) {
    int                w                          = (int)lvgl_port_width();
    static const char* tab_labels[VIEW_TAB_COUNT] = {"Settings", "Nodes", "DM", "Channel"};

    add_rect(scr, 0, 0, w, TAB_BAR_H, COL_PAGER_BG);
    add_rect(scr, 0, TAB_BAR_H - 1, w, 1, COL_PAGER_ACCENT);

    int label_y = (TAB_BAR_H - TXT_TAB) / 2;
    int x       = 12;

    const char* vname = (current_view >= 0 && current_view < VIEW_TAB_COUNT) ? tab_labels[current_view] : "";
    if (vname[0]) {
        add_label(scr, x, label_y, TXT_TAB, COL_PAGER_TEXT, vname);
        x += text_w(vname, TXT_TAB) + 12;
    }

    int badge_y   = (TAB_BAR_H - TXT_SMALL) / 2 - 2;
    int badge_h   = TXT_SMALL + 4;
    int dm_unread = contact_unread_total();
    if (dm_unread > 0 && current_view != VIEW_CHAT) {
        char buf[8];
        snprintf(buf, sizeof(buf), "DM %d", dm_unread > 99 ? 99 : dm_unread);
        int bw = text_w(buf, TXT_SMALL) + 12;
        add_rect(scr, x, badge_y, bw, badge_h, COL_RED);
        add_label(scr, x + 6, badge_y + 2, TXT_SMALL, COL_PAGER_BG, buf);
        x += bw + 8;
    }
    int ch_unread = channel_unread_total();
    if (ch_unread > 0 && current_view != VIEW_CHANNEL) {
        char buf[8];
        snprintf(buf, sizeof(buf), "# %d", ch_unread > 99 ? 99 : ch_unread);
        int bw = text_w(buf, TXT_SMALL) + 12;
        add_rect(scr, x, badge_y, bw, badge_h, COL_RED);
        add_label(scr, x + 6, badge_y + 2, TXT_SMALL, COL_PAGER_BG, buf);
        x += bw + 8;
    }

    int status_y = (TAB_BAR_H - TXT_BODY) / 2;
    home_status_right(scr, w - 12, status_y, TXT_BODY);
}

// ── VIEW_NODES ───────────────────────────────────────────────────────────────
// Pixel-matched port of render_nodes.c. Scrolling contacts+nodes list with row
// selection. The QR overlay reachable from this view (Q) is left on the PAX
// path: lvgl_view_active(VIEW_NODES) reports false while qr_overlay_active, so
// render() routes the whole nodes+QR frame through the proven PAX renderer.

#define NODES_ROW_H    44
#define NODES_Y0       (TAB_BAR_H + 4)
#define NODES_HEADER_H 26

static void render_nodes_lvgl(void) {
    int       w   = (int)lvgl_port_width();
    int       h   = (int)lvgl_port_height();
    lv_obj_t* scr = begin_screen(COL_BG);
    pt_reset();

    tab_bar_lvgl(scr);

    int hdr_y = NODES_Y0;
    add_rect(scr, 0, hdr_y, w, NODES_HEADER_H, COL_HEADER);
    add_rect(scr, 0, hdr_y + NODES_HEADER_H - 1, w, 1, COL_PANEL);

    int age_col_w  = 60;
    int dist_col_w = 60;
    int pkts_col_w = 60;
    int snr_col_w  = 54;
    int rssi_col_w = 64;
    int age_hdr_x  = w - age_col_w - 6;
    int dist_hdr_x = age_hdr_x - dist_col_w;
    int pkts_hdr_x = dist_hdr_x - pkts_col_w;
    int snr_hdr_x  = pkts_hdr_x - snr_col_w;
    int rssi_hdr_x = snr_hdr_x - rssi_col_w;
    int hdr_text_y = hdr_y + (NODES_HEADER_H - TXT_SMALL) / 2;
    add_label(scr, 8, hdr_text_y, TXT_SMALL, COL_WHITE, "Role");
    add_label(scr, 96, hdr_text_y, TXT_SMALL, COL_WHITE, "Name");
    add_label(scr, rssi_hdr_x, hdr_text_y, TXT_SMALL, COL_WHITE, "RSSI");
    add_label(scr, snr_hdr_x, hdr_text_y, TXT_SMALL, COL_WHITE, "SNR");
    add_label(scr, pkts_hdr_x, hdr_text_y, TXT_SMALL, COL_WHITE, "#Pkt");
    add_label(scr, dist_hdr_x, hdr_text_y, TXT_SMALL, COL_WHITE, "Dist");
    add_label(scr, age_hdr_x, hdr_text_y, TXT_SMALL, COL_WHITE, "Seen");

    int      list_y0    = NODES_Y0 + NODES_HEADER_H;
    int      footer_h   = 60;
    int      list_h     = h - footer_h - list_y0;
    int      rows_vis   = list_h / NODES_ROW_H;
    uint32_t now_ms     = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    int      row_text_y = (NODES_ROW_H - TXT_BODY) / 2;

    if (!lora_rx_ok) {
        add_label(scr, 12, list_y0 + 14, TXT_BODY, COL_AMBER, "LoRa radio not available");
        add_label(scr, 12, list_y0 + 14 + TXT_BODY + 8, TXT_BODY, COL_GRAY,
                  "Update via Launcher: Tools > Firmware update");
    } else if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (node_count == 0 && contact_count == 0) {
            add_label(scr, 12, list_y0 + 14, TXT_BODY, COL_GRAY, "Listening... no nodes heard yet.");
        } else {
            display_row_t rows_dl[MAX_CONTACTS + MAX_NODES];
            int           idx_count = build_node_display(rows_dl, MAX_CONTACTS + MAX_NODES);

            int max_scroll = idx_count - rows_vis;
            if (max_scroll < 0) max_scroll = 0;
            if (node_scroll > max_scroll) node_scroll = max_scroll;
            if (node_scroll < 0) node_scroll = 0;

            if (node_cursor >= idx_count) node_cursor = idx_count > 0 ? idx_count - 1 : 0;
            if (node_cursor < 0) node_cursor = 0;
            if (node_cursor < node_scroll) node_scroll = node_cursor;
            if (node_cursor >= node_scroll + rows_vis) node_scroll = node_cursor - rows_vis + 1;

            bool divider_drawn = false;
            for (int row = 0; row < rows_vis; row++) {
                int list_idx = row + node_scroll;
                if (list_idx >= idx_count) break;
                display_row_t* d = &rows_dl[list_idx];
                node_entry_t*  n = (d->node_idx >= 0) ? &node_list[d->node_idx] : NULL;

                int  y         = list_y0 + row * NODES_ROW_H;
                bool is_cursor = (list_idx == node_cursor);

                if (!divider_drawn && !d->is_contact) {
                    bool prev_was_contact       = (row > 0 && rows_dl[list_idx - 1].is_contact);
                    bool scrolled_past_contacts = (node_scroll > 0 && contact_count > 0 && list_idx <= contact_count);
                    if (prev_was_contact || scrolled_past_contacts) {
                        add_rect(scr, 6, y - 2, w - 12, 2, COL_AMBER);
                    }
                    divider_drawn = true;
                }

                if (is_cursor) {
                    add_rect(scr, 0, y, w, NODES_ROW_H, COL_PANEL);
                    add_rect(scr, 0, y, 5, NODES_ROW_H, COL_ACCENT);
                }

                int age_x  = w - age_col_w - 6;
                int dist_x = age_x - dist_col_w;
                int pkts_x = dist_x - pkts_col_w;
                int snr_x  = pkts_x - snr_col_w;
                int rssi_x = snr_x - rssi_col_w;

                char age_buf[20];
                if (n) {
                    uint32_t age_s = (now_ms - n->last_seen_ms) / 1000;
                    if (age_s < 60)
                        snprintf(age_buf, sizeof(age_buf), "%lus", (unsigned long)age_s);
                    else if (age_s < 3600)
                        snprintf(age_buf, sizeof(age_buf), "%lum", (unsigned long)(age_s / 60));
                    else
                        snprintf(age_buf, sizeof(age_buf), "%luh", (unsigned long)(age_s / 3600));
                } else {
                    snprintf(age_buf, sizeof(age_buf), "--");
                }
                add_label(scr, age_x, y + row_text_y, TXT_BODY, COL_GRAY, age_buf);

                char dist_buf[12];
                if (n && n->position_valid && gps_position_valid) {
                    const double k    = 1e-6 * (M_PI / 180.0);
                    double       lat1 = (double)gps_lat_e6 * k;
                    double       lon1 = (double)gps_lon_e6 * k;
                    double       lat2 = (double)n->lat * k;
                    double       lon2 = (double)n->lon * k;
                    double       xx   = (lon2 - lon1) * cos((lat1 + lat2) * 0.5);
                    double       yy   = (lat2 - lat1);
                    double       d_km = 6371.0 * sqrt(xx * xx + yy * yy);
                    if (d_km < 1.0)
                        snprintf(dist_buf, sizeof(dist_buf), "%dm", (int)(d_km * 1000.0));
                    else if (d_km < 10.0)
                        snprintf(dist_buf, sizeof(dist_buf), "%.1fkm", d_km);
                    else
                        snprintf(dist_buf, sizeof(dist_buf), "%dkm", (int)d_km);
                } else {
                    snprintf(dist_buf, sizeof(dist_buf), "--");
                }
                add_label(scr, dist_x, y + row_text_y, TXT_BODY, COL_GRAY, dist_buf);

                char pkts_buf[8];
                if (n)
                    snprintf(pkts_buf, sizeof(pkts_buf), "#%d", n->packet_count);
                else
                    snprintf(pkts_buf, sizeof(pkts_buf), "--");
                add_label(scr, pkts_x, y + row_text_y, TXT_BODY, COL_GRAY, pkts_buf);

                char     rssi_buf[8], snr_buf[8];
                uint32_t rssi_col, snr_col;
                if (n && n->stats_valid) {
                    int rssi_dbm = (int)n->last_rssi_dbm;
                    int snr_dB   = (int)n->last_snr_db_x4 / 4;
                    snprintf(rssi_buf, sizeof(rssi_buf), "%d", rssi_dbm);
                    snprintf(snr_buf, sizeof(snr_buf), "%+d", snr_dB);
                    rssi_col = (rssi_dbm >= -80) ? COL_GREEN : (rssi_dbm >= -105) ? COL_AMBER : COL_RED;
                    snr_col  = (snr_dB >= 0) ? COL_GREEN : (snr_dB >= -10) ? COL_AMBER : COL_RED;
                } else {
                    snprintf(rssi_buf, sizeof(rssi_buf), "--");
                    snprintf(snr_buf, sizeof(snr_buf), "--");
                    rssi_col = COL_GRAY;
                    snr_col  = COL_GRAY;
                }
                add_label(scr, rssi_x, y + row_text_y, TXT_BODY, rssi_col, rssi_buf);
                add_label(scr, snr_x, y + row_text_y, TXT_BODY, snr_col, snr_buf);

                meshcore_device_role_t role     = n ? n->role : (meshcore_device_role_t)contacts[d->contact_idx].role;
                const char*            src_name = n ? n->name : contacts[d->contact_idx].alias;

                const char* rl       = role_label(role);
                uint32_t    role_col = (role == MESHCORE_DEVICE_ROLE_REPEATER)      ? COL_BLUE
                                       : (role == MESHCORE_DEVICE_ROLE_ROOM_SERVER) ? 0xFFBB9AF7
                                       : (role == MESHCORE_DEVICE_ROLE_SENSOR)      ? COL_AMBER
                                                                                    : COL_GREEN;
                add_label(scr, 8, y + row_text_y, TXT_BODY, role_col, rl);

                int name_x = 96;
                if (d->is_contact) {
                    add_label(scr, 78, y + row_text_y, TXT_BODY, COL_AMBER, "*");
                }

                int row_unread   = d->is_contact ? contact_unread[d->contact_idx] : 0;
                int badge_w_resv = 0;
                if (row_unread > 0) {
                    char ub[8];
                    snprintf(ub, sizeof(ub), "%d", row_unread > 99 ? 99 : row_unread);
                    badge_w_resv = text_w(ub, TXT_SMALL) + 12 + 6;
                }

                char name_trunc[25];
                int  max_name_w = rssi_x - name_x - 6 - badge_w_resv;
                int  max_chars  = max_name_w / 11;
                if (max_chars > 24) max_chars = 24;
                if (max_chars < 1) max_chars = 1;
                snprintf(name_trunc, sizeof(name_trunc), "%.*s", max_chars, src_name);
                uint32_t name_col = is_cursor ? COL_WHITE : (n == NULL ? COL_GRAY : COL_WHITE);
                add_label(scr, name_x, y + row_text_y, TXT_BODY, name_col, name_trunc);

                if (row_unread > 0) {
                    char ub[8];
                    snprintf(ub, sizeof(ub), "%d", row_unread > 99 ? 99 : row_unread);
                    int nw = text_w(name_trunc, TXT_BODY);
                    int bw = text_w(ub, TXT_SMALL) + 12;
                    int bx = name_x + nw + 6;
                    int by = y + (NODES_ROW_H - (TXT_SMALL + 4)) / 2;
                    add_rect(scr, bx, by, bw, TXT_SMALL + 4, COL_RED);
                    add_label(scr, bx + 6, by + 2, TXT_SMALL, COL_HEADER, ub);
                }

                add_rect(scr, 12, y + NODES_ROW_H - 1, w - 24, 1, COL_PANEL);
            }

            if (idx_count > rows_vis) {
                char sc[24];
                snprintf(sc, sizeof(sc), "%d/%d", node_scroll + 1, idx_count);
                add_label(scr, w - text_w(sc, TXT_SMALL) - 10, h - footer_h - TXT_SMALL - 2, TXT_SMALL, COL_GRAY, sc);
            }
            if (idx_count == 0 && (node_count > 0 || contact_count > 0)) {
                add_label(scr, 12, list_y0 + 14, TXT_BODY, COL_AMBER,
                          "No entries match the active filter -- press L to clear");
            }
        }
        xSemaphoreGive(node_mutex);
    }

    int fy_base = h - footer_h;
    add_rect(scr, 0, fy_base, w, footer_h, COL_HEADER);
    add_rect(scr, 0, fy_base, w, 1, COL_PANEL);

    int  fx      = 10;
    int  fy_text = fy_base + 6;
    char counts[48];
    snprintf(counts, sizeof(counts), "Nodes:%d  Contacts:%d", node_count, contact_count);
    add_label(scr, fx, fy_text, TXT_BODY, COL_WHITE, counts);
    fx += text_w(counts, TXT_BODY) + 20;

    if (node_filter != MESHCORE_DEVICE_ROLE_UNKNOWN) {
        char pill[40];
        snprintf(pill, sizeof(pill), "filter: %s", role_label(node_filter));
        int pw = text_w(pill, TXT_BODY);
        add_rect(scr, fx - 6, fy_text - 2, pw + 12, TXT_BODY + 4, COL_AMBER);
        add_label(scr, fx, fy_text, TXT_BODY, COL_HEADER, pill);
        fx += pw + 22;
    }

    const char* ctrl   = (node_filter == MESHCORE_DEVICE_ROLE_UNKNOWN) ? "W/S nav   A:advert   F:fav   L:filter   Q:QR"
                                                                       : "L:next   F:fav   A:advert";
    int         ctrl_y = fy_text + (TXT_BODY - TXT_SMALL) / 2;
    add_label(scr, fx, ctrl_y, TXT_SMALL, COL_HINT, ctrl);
    add_back_hint(scr, fx + text_w(ctrl, TXT_SMALL) + 16, ctrl_y, ": home", TXT_SMALL);

    if (identity_is_ready()) {
        uint32_t now_ms2 = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        char     adv_buf[48];
        if (last_advert_ms == 0) {
            snprintf(adv_buf, sizeof(adv_buf), "advert: pending");
        } else {
            uint32_t age_s = (now_ms2 - last_advert_ms) / 1000;
            snprintf(adv_buf, sizeof(adv_buf), "last advert: %lus ago", (unsigned long)age_s);
        }
        add_label(scr, 10, fy_text + TXT_BODY + 6, TXT_SMALL, COL_GRAY, adv_buf);
    }
}

// ── Dispatch ─────────────────────────────────────────────────────────────────

bool lvgl_view_active(app_view_t v) {
    switch (v) {
        case VIEW_ABOUT:
        case VIEW_HOME:
        case VIEW_TOOLBOX:
        case VIEW_TOOLBOX_COVERAGE:
            return true;
        case VIEW_NODES:
            // The QR overlay reachable from the nodes list is not yet ported to
            // LVGL; defer the whole nodes+QR frame to the PAX path while it's up
            // so the overlay keeps rendering through the proven renderer.
            return !qr_overlay_active;
        default:
            return false;
    }
}

void lvgl_view_render(app_view_t v) {
    switch (v) {
        case VIEW_ABOUT:
            render_about_lvgl();
            break;
        case VIEW_HOME:
            render_home_lvgl();
            break;
        case VIEW_TOOLBOX:
            render_toolbox_lvgl();
            break;
        case VIEW_TOOLBOX_COVERAGE:
            render_toolbox_coverage_lvgl();
            break;
        case VIEW_NODES:
            render_nodes_lvgl();
            break;
        default:
            return;
    }
    lvgl_port_refresh_now();
}
