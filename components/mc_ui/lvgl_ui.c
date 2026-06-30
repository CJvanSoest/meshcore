// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "lvgl_ui.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "app_config.h"
#include "bsp/power.h"
#include "channels.h"
#include "chat.h"
#include "contacts.h"
#include "coverage.h"
#include "emoji_table.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gps_task.h"
#include "identity.h"
#include "lvgl.h"
#include "lvgl_port.h"
#include "map.h"
#include "nodes.h"
#include "qrcodegen.h"
#include "radio.h"
#include "region_limits.h"
#include "render.h"  // COL_* palette + TXT_* sizes (shared with the PAX views)
#include "render_internal.h"
#include "settings_nvs.h"
#include "ui_state.h"
#include "wifi_connection.h"

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

// ── Inline emoji (chat) ──────────────────────────────────────────────────────
// The 32x32 Twemoji bitmaps in emoji_bitmaps.c are ARGB8888 (0xAARRGGBB); on
// this little-endian target that byte order is exactly LVGL's
// LV_COLOR_FORMAT_ARGB8888 (B,G,R,A), so each flash array wraps straight into an
// lv_image_dsc_t with no copy — the mirror of emoji.c's pax_buf wrappers.

extern const uint32_t* const EMOJI_BITMAPS[];
#define EMOJI_PX 32

static lv_image_dsc_t s_emoji_dsc[EMOJI_COUNT];
static bool           s_emoji_dsc_ready;

static void emoji_dsc_init(void) {
    if (s_emoji_dsc_ready) {
        return;
    }
    for (int i = 0; i < EMOJI_COUNT; i++) {
        s_emoji_dsc[i].header.magic  = LV_IMAGE_HEADER_MAGIC;
        s_emoji_dsc[i].header.cf     = LV_COLOR_FORMAT_ARGB8888;
        s_emoji_dsc[i].header.flags  = 0;
        s_emoji_dsc[i].header.w      = EMOJI_PX;
        s_emoji_dsc[i].header.h      = EMOJI_PX;
        s_emoji_dsc[i].header.stride = EMOJI_PX * 4;
        s_emoji_dsc[i].data          = (const uint8_t*)EMOJI_BITMAPS[i];
        s_emoji_dsc[i].data_size     = EMOJI_PX * EMOJI_PX * 4;
    }
    s_emoji_dsc_ready = true;
}

// Inline emoji diameter for a given text size — matches emoji.c exactly.
static int emoji_inline_d(int size) {
    int d = (int)(size * 1.1f);
    if (d < 12) d = 12;
    return d;
}

// Draw (parent != NULL) or just measure (parent == NULL) `text` at (x, y) with
// inline emoji, returning the advance width. Faithful port of emoji.c's
// render_or_measure so the wrap/measure math lines up 1:1 with what's drawn.
static int emoji_text(lv_obj_t* parent, int x, int y, int size, uint32_t col, const char* text) {
    emoji_dsc_init();
    int  dx = 0;
    int  d  = emoji_inline_d(size);
    int  r  = d / 2;
    char run[256];
    int  run_len = 0;

    int i = 0;
    while (text[i]) {
        uint32_t cp  = 0;
        int      adv = utf8_decode(&text[i], &cp);
        if (adv <= 0) {
            if (run_len < (int)sizeof(run) - 1) run[run_len++] = '?';
            i++;
            continue;
        }
        int idx = (cp >= 0x80) ? emoji_lookup_by_codepoint(cp) : -1;
        if (idx >= 0) {
            if (run_len > 0) {
                run[run_len] = '\0';
                if (parent) add_label(parent, x + dx, y, size, col, run);
                dx      += text_w(run, size);
                run_len  = 0;
            }
            if (parent) {
                lv_obj_t* im = lv_image_create(parent);
                lv_image_set_src(im, &s_emoji_dsc[idx]);
                lv_image_set_antialias(im, true);
                lv_image_set_pivot(im, 0, 0);
                lv_image_set_scale(im, (256 * d) / EMOJI_PX);
                lv_obj_set_pos(im, x + dx, y + size / 2 - r);
            }
            dx += d + 1;
            i  += adv;
            continue;
        }
        if (cp >= 0x80) {
            if (run_len < (int)sizeof(run) - 1) run[run_len++] = '?';
            i += adv;
            continue;
        }
        if (run_len < (int)sizeof(run) - 1) run[run_len++] = (char)cp;
        i += adv;
    }
    if (run_len > 0) {
        run[run_len] = '\0';
        if (parent) add_label(parent, x + dx, y, size, col, run);
        dx += text_w(run, size);
    }
    return dx;
}

// Draw emoji `idx` centred at (cx, cy) with diameter `d` — the LVGL mirror of
// emoji.c's emoji_draw (top-left at cx-d/2, cy-d/2, scaled from EMOJI_PX).
static void emoji_image(lv_obj_t* parent, int idx, int cx, int cy, int d) {
    if (idx < 0 || idx >= EMOJI_COUNT) {
        return;
    }
    emoji_dsc_init();
    lv_obj_t* im = lv_image_create(parent);
    lv_image_set_src(im, &s_emoji_dsc[idx]);
    lv_image_set_antialias(im, true);
    lv_image_set_pivot(im, 0, 0);
    lv_image_set_scale(im, (256 * d) / EMOJI_PX);
    lv_obj_set_pos(im, cx - d / 2, cy - d / 2);
}

// Paged 4x5 emoji-picker overlay drawn on top of the Chat/Channel base view
// while typing. Pixel-matched port of render_emoji_picker_overlay() in render.c.
static void render_emoji_picker_overlay_lvgl(lv_obj_t* scr, int w, int h) {
    const int cols     = 4;
    const int vis_rows = 5;
    const int per_page = cols * vis_rows;
    const int cell     = 48;
    const int pad      = 14;
    const int panel_w  = cols * cell + 2 * pad;
    const int panel_h  = vis_rows * cell + 2 * pad + TXT_SMALL + 6;
    int       panel_x  = (w - panel_w) / 2;
    int       panel_y  = h - CHAT_INPUT_H - FOOTER_H - panel_h - 4;
    if (panel_y < TAB_BAR_H + 4) panel_y = TAB_BAR_H + 4;

    if (emoji_picker_cursor < 0) emoji_picker_cursor = 0;
    if (emoji_picker_cursor >= EMOJI_COUNT) emoji_picker_cursor = EMOJI_COUNT - 1;
    int pages = (EMOJI_COUNT + per_page - 1) / per_page;
    int page  = emoji_picker_cursor / per_page;
    int start = page * per_page;

    add_rect(scr, panel_x, panel_y, panel_w, panel_h, COL_HEADER);
    add_rect(scr, panel_x, panel_y, panel_w, 2, COL_ACCENT);

    char title[40];
    if (pages > 1) {
        snprintf(title, sizeof(title), "Pick emoji  %d/%d", page + 1, pages);
    } else {
        snprintf(title, sizeof(title), "Pick emoji");
    }
    add_label(scr, panel_x + pad, panel_y + 4, TXT_SMALL, COL_AMBER, title);
    if (pages > 1) {
        const char* nav = "W/S: scroll";
        add_label(scr, panel_x + panel_w - text_w(nav, TXT_SMALL) - 6, panel_y + 4, TXT_SMALL, COL_GRAY, nav);
    }

    int grid_x = panel_x + pad;
    int grid_y = panel_y + 6 + TXT_SMALL;

    for (int i = start; i < start + per_page && i < EMOJI_COUNT; i++) {
        int  local = i - start;
        int  r     = local / cols;
        int  c     = local % cols;
        int  cx    = grid_x + c * cell + cell / 2;
        int  cy    = grid_y + r * cell + cell / 2;
        bool sel   = (i == emoji_picker_cursor);
        if (sel) {
            add_rect(scr, cx - cell / 2 + 2, cy - cell / 2 + 2, cell - 4, cell - 4, COL_PANEL);
        }
        emoji_image(scr, i, cx, cy, 2 * (cell / 2 - 6));
    }
}

// ── QR overlay (contact / owntracks / channel) ───────────────────────────────
// Full-screen overlay drawn on top of the Nodes/Channel base view. Pixel-matched
// port of render_qr_overlay() in render_nodes.c. The qrcodegen module produces a
// module bitmap; we wrap it at module resolution into an lv_image_dsc_t and
// upscale by an integer factor (nearest-neighbour, antialias off) so the cells
// stay crisp without materialising one lv_obj per module.

#define QR_MAX_SIZE 77  // qrcodegen version 15 → 17 + 4*15

static uint32_t       s_qr_argb[QR_MAX_SIZE * QR_MAX_SIZE];
static lv_image_dsc_t s_qr_dsc;

// Centred label helper for the QR overlay captions.
static void add_label_centered(lv_obj_t* scr, int w, int y, int size, uint32_t col, const char* text) {
    add_label(scr, (w - text_w(text, size)) / 2, y, size, col, text);
}

static void render_qr_overlay_lvgl(void) {
    int       w   = (int)lvgl_port_width();
    int       h   = (int)lvgl_port_height();
    lv_obj_t* scr = begin_screen(COL_BG);
    pt_reset();

    char        url[256];
    const char* title_label  = NULL;
    char        subtitle[96] = {0};

    if (qr_overlay_mode == QR_MODE_OWNTRACKS) {
        const char* host         = "tanmatsu.local";
        char        host_buf[24] = {0};
        if (wifi_connection_is_connected()) {
            esp_netif_ip_info_t* ip = wifi_get_ip_info();
            if (ip && ip->ip.addr) {
                snprintf(host_buf, sizeof(host_buf), IPSTR, IP2STR(&ip->ip));
                host = host_buf;
            }
        }
        snprintf(url, sizeof(url), "https://%s:8443/ping?key=%s", host, http_api_key[0] ? http_api_key : "(unset)");
        title_label = "Scan for OwnTracks";
        snprintf(subtitle, sizeof(subtitle), "https://%s:8443/ping", host);
    } else if (qr_overlay_mode == QR_MODE_CHANNEL) {
        const channel_t* ch =
            (qr_channel_idx >= 0 && qr_channel_idx < channel_count) ? &channels[qr_channel_idx] : NULL;
        char hex_key[2 * CHANNEL_SECRET_LEN + 1] = {0};
        char enc_name[72]                        = {0};
        int  ei                                  = 0;
        if (ch) {
            for (int i = 0; i < CHANNEL_SECRET_LEN; i++) snprintf(&hex_key[i * 2], 3, "%02x", ch->secret[i]);
            static const char hexd[] = "0123456789abcdef";
            for (int i = 0; ch->name[i] && ei < (int)sizeof(enc_name) - 4; i++) {
                unsigned char c = (unsigned char)ch->name[i];
                if (c == ' ') {
                    enc_name[ei++] = '+';
                } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                           c == '_' || c == '.') {
                    enc_name[ei++] = (char)c;
                } else {
                    enc_name[ei++] = '%';
                    enc_name[ei++] = hexd[c >> 4];
                    enc_name[ei++] = hexd[c & 0xf];
                }
            }
        }
        snprintf(url, sizeof(url), "meshcore://channel/add?name=%s&secret=%s", enc_name, hex_key);
        title_label = "Scan to add channel";
        snprintf(subtitle, sizeof(subtitle), "%s", ch ? ch->name : "(no channel)");
    } else {
        char hex_key[65];
        for (int i = 0; i < 32; i++) snprintf(&hex_key[i * 2], 3, "%02x", node_pub_key[i]);
        hex_key[64] = '\0';

        const char* adv_src =
            lora_advert_name[0] ? lora_advert_name : ((owner_name[0] && owner_name[0] != '(') ? owner_name : "");

        char encoded_name[64];
        int  ei = 0;
        for (int i = 0; adv_src[i] && ei < 62; i++) {
            char c = adv_src[i];
            if (c == ' ') {
                encoded_name[ei++] = '+';
            } else {
                encoded_name[ei++] = c;
            }
        }
        encoded_name[ei] = '\0';

        snprintf(url, sizeof(url), "meshcore://contact/add?name=%s&public_key=%s&type=1", encoded_name, hex_key);
        title_label = "Scan to add contact";
        snprintf(subtitle, sizeof(subtitle), "%s", adv_src[0] ? adv_src : "(no name)");
    }

    static uint8_t     qr_data[qrcodegen_BUFFER_LEN_MAX];
    static uint8_t     tmp_buf[qrcodegen_BUFFER_LEN_MAX];
    enum qrcodegen_Ecc ecc = (qr_overlay_mode == QR_MODE_OWNTRACKS) ? qrcodegen_Ecc_LOW : qrcodegen_Ecc_MEDIUM;
    bool ok = qrcodegen_encodeText(url, tmp_buf, qr_data, ecc, qrcodegen_VERSION_MIN, 15, qrcodegen_Mask_AUTO, true);

    if (!ok) {
        add_label(scr, 20, h / 2, TXT_BODY, COL_AMBER, "QR encode failed");
        return;
    }

    int qr_size = qrcodegen_getSize(qr_data);
    if (qr_size > QR_MAX_SIZE) {
        add_label(scr, 20, h / 2, TXT_BODY, COL_AMBER, "QR too large");
        return;
    }
    int max_px  = (h * 6) / 10;
    int cell_px = max_px / qr_size;
    if (cell_px < 2) cell_px = 2;
    int qr_px = cell_px * qr_size;
    int qr_x  = (w - qr_px) / 2;
    int qr_y  = (h - qr_px) / 2;

    int margin = cell_px * 2;
    // White quiet-zone behind the code; the code image carries its own white
    // background for the 0-modules so the two read as one clean block.
    add_rect(scr, qr_x - margin, qr_y - margin, qr_px + margin * 2, qr_px + margin * 2, 0xFFFFFFFF);

    for (int row = 0; row < qr_size; row++) {
        for (int col = 0; col < qr_size; col++) {
            s_qr_argb[row * qr_size + col] = qrcodegen_getModule(qr_data, col, row) ? 0xFF000000u : 0xFFFFFFFFu;
        }
    }
    s_qr_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    s_qr_dsc.header.cf     = LV_COLOR_FORMAT_ARGB8888;
    s_qr_dsc.header.flags  = 0;
    s_qr_dsc.header.w      = qr_size;
    s_qr_dsc.header.h      = qr_size;
    s_qr_dsc.header.stride = qr_size * 4;
    s_qr_dsc.data          = (const uint8_t*)s_qr_argb;
    s_qr_dsc.data_size     = (uint32_t)(qr_size * qr_size * 4);

    lv_obj_t* im = lv_image_create(scr);
    lv_image_set_src(im, &s_qr_dsc);
    lv_image_set_antialias(im, false);
    lv_image_set_pivot(im, 0, 0);
    lv_image_set_scale(im, 256 * cell_px);
    lv_obj_set_pos(im, qr_x, qr_y);

    add_label_centered(scr, w, qr_y - margin - TXT_TITLE - 6, TXT_TITLE, COL_AMBER, title_label);
    add_label_centered(scr, w, qr_y + qr_px + margin + 6, TXT_SMALL, COL_GRAY, subtitle);

    int next_y = qr_y + qr_px + margin + 6 + TXT_SMALL + 6;
    if (qr_overlay_mode == QR_MODE_OWNTRACKS && http_api_key[0]) {
        char key_preview[40];
        snprintf(key_preview, sizeof(key_preview), "key %.8s...%.4s", http_api_key, http_api_key + 60);
        add_label_centered(scr, w, next_y, TXT_SMALL, COL_GRAY, key_preview);
        next_y += TXT_SMALL + 6;
    } else if (qr_overlay_mode == QR_MODE_CHANNEL) {
        const channel_t* ch =
            (qr_channel_idx >= 0 && qr_channel_idx < channel_count) ? &channels[qr_channel_idx] : NULL;
        if (ch) {
            char secret_hex[2 * CHANNEL_SECRET_LEN + 1] = {0};
            for (int i = 0; i < CHANNEL_SECRET_LEN; i++) snprintf(&secret_hex[i * 2], 3, "%02x", ch->secret[i]);
            add_label_centered(scr, w, next_y, TXT_SMALL, COL_GRAY, secret_hex);
            next_y += TXT_SMALL + 6;
        }
    }

    add_label_centered(scr, w, next_y, TXT_SMALL, COL_AMBER, "Press the red X to close");
}

// ── Shared chat-message ring renderer (DM + Channel) ─────────────────────────
// Pixel-matched port of render_msg_list() / msg_wrap() in render_chat.c.

#define MSG_MAX_LINES 8

// Greedy word-wrap to fit max_w px at TXT_BODY, measuring with the same inline-
// emoji metric used to draw, so a bubble always fits its own content.
static int msg_wrap_lvgl(const char* text, int max_w, char out[][MAX_MSG_TEXT], int max_lines) {
    int         nl                 = 0;
    char        line[MAX_MSG_TEXT] = {0};
    int         ll                 = 0;
    const char* p                  = text;
    while (*p && nl < max_lines) {
        if (*p == '\n' || *p == '\r') {
            memcpy(out[nl], line, ll + 1);
            nl++;
            ll      = 0;
            line[0] = 0;
            while (*p == '\n' || *p == '\r') p++;
            continue;
        }
        const char* w0 = p;
        while (*p && *p != ' ' && *p != '\n' && *p != '\r') p++;
        while (*p == ' ') p++;
        int wlen = (int)(p - w0);
        if (wlen > MAX_MSG_TEXT - 1) wlen = MAX_MSG_TEXT - 1;

        char cand[MAX_MSG_TEXT];
        int  copy = wlen;
        if (ll + copy > MAX_MSG_TEXT - 1) copy = MAX_MSG_TEXT - 1 - ll;
        if (ll) memcpy(cand, line, ll);
        memcpy(cand + ll, w0, copy);
        cand[ll + copy] = 0;

        if (ll == 0 || emoji_text(NULL, 0, 0, TXT_BODY, 0, cand) <= max_w) {
            memcpy(line, cand, ll + copy + 1);
            ll += copy;
        } else {
            memcpy(out[nl], line, ll + 1);
            nl++;
            if (nl >= max_lines) break;
            ll = (wlen < MAX_MSG_TEXT) ? wlen : MAX_MSG_TEXT - 1;
            memcpy(line, w0, ll);
            line[ll] = 0;
        }
    }
    if (ll > 0 && nl < max_lines) {
        memcpy(out[nl], line, ll + 1);
        nl++;
    }
    if (nl == 0) {
        out[0][0] = 0;
        nl        = 1;
    }
    return nl;
}

static void render_msg_list_lvgl(lv_obj_t* scr, int w, int list_y0, int list_h, chat_msg_t* msgs, int head, int count,
                                 int* scroll_p, bool is_channel) {
    if (count == 0) {
        add_label(scr, 14, list_y0 + 10, TXT_BODY, COL_GRAY, "No messages yet. Press T to type.");
        return;
    }
    int sc = *scroll_p;
    if (sc > count) sc = count;
    if (sc < 1) sc = 1;
    *scroll_p = sc;

    const int line_h  = TXT_BODY + 4;
    const int meta_h  = TXT_TINY + 4;
    const int pad_x   = 8;
    const int pad_y   = 5;
    const int gap     = 10;
    const int margin  = 14;
    const int avail_w = w - 2 * margin - 2 * pad_x;
    char      lines[MSG_MAX_LINES][MAX_MSG_TEXT];

    // Clip container at the list region: LVGL clips children to it, so a tall
    // top bubble can't bleed into the header (the PAX path used pax_clip). All
    // child coordinates below are local to this container.
    lv_obj_t* lst = lv_obj_create(scr);
    lv_obj_remove_style_all(lst);
    lv_obj_set_pos(lst, 0, list_y0);
    lv_obj_set_size(lst, w, list_h);
    lv_obj_clear_flag(lst, LV_OBJ_FLAG_SCROLLABLE);

    int y = list_h;
    for (int li = sc - 1; li >= 0 && y > 0; li--) {
        int         ring = (head - count + li + MAX_CHAT_MSGS * 2) % MAX_CHAT_MSGS;
        chat_msg_t* m    = &msgs[ring];
        if (!m->active) continue;

        int nl = msg_wrap_lvgl(m->text, avail_w, lines, MSG_MAX_LINES);

        char meta[64] = {0};
        {
            char tbuf[12] = {0};
            if (m->timestamp_unix > 0) {
                time_t    t = (time_t)m->timestamp_unix;
                struct tm lt;
                localtime_r(&t, &lt);
                snprintf(tbuf, sizeof(tbuf), "%02d:%02d", lt.tm_hour, lt.tm_min);
            }
            char hbuf[16] = {0};
            if (!m->is_mine && m->hops != 0xFF) snprintf(hbuf, sizeof(hbuf), "%uh", (unsigned)m->hops);
            const char* ack = NULL;
            if (m->is_mine) {
                if (m->ack_state == 1)
                    ack = is_channel ? "sent" : "...";
                else if (m->ack_state == 2)
                    ack = is_channel ? "relayed" : "ack";
                else if (m->ack_state == 3)
                    ack = "not sent";
            }
            int o = 0;
            if (tbuf[0]) o += snprintf(meta + o, sizeof(meta) - o, "%s", tbuf);
            if (hbuf[0]) o += snprintf(meta + o, sizeof(meta) - o, "%s%s", o ? " - " : "", hbuf);
            if (ack) snprintf(meta + o, sizeof(meta) - o, "%s%s", o ? " - " : "", ack);
        }

        int bubble_h = pad_y + nl * line_h + meta_h + pad_y;
        int mh       = bubble_h + gap;

        y -= mh;
        if (y + mh <= 0) break;

        int bubble_y = y + gap / 2;

        int maxw = 0;
        for (int k = 0; k < nl; k++) {
            int lw = emoji_text(NULL, 0, 0, TXT_BODY, 0, lines[k]);
            if (lw > maxw) maxw = lw;
        }
        if (meta[0] || m->is_mine) {
            const char* ml = meta[0] ? meta : "You";
            int         mw = text_w(ml, TXT_TINY);
            if (mw > maxw) maxw = mw;
        }
        int max_bubble_w = w - 2 * margin;
        int bubble_w     = maxw + 2 * pad_x;
        if (bubble_w > max_bubble_w) bubble_w = max_bubble_w;

        if (m->is_mine) {
            int bx = w - margin - bubble_w;
            add_rect(lst, bx, bubble_y, bubble_w, bubble_h, COL_PANEL);
            for (int k = 0; k < nl; k++) {
                int lw = emoji_text(NULL, 0, 0, TXT_BODY, 0, lines[k]);
                emoji_text(lst, bx + bubble_w - pad_x - lw, bubble_y + pad_y + k * line_h, TXT_BODY, COL_BLUE,
                           lines[k]);
            }
            uint32_t    mcol = (m->ack_state == 2) ? COL_GREEN : (m->ack_state == 3) ? COL_RED : COL_GRAY;
            const char* ml   = meta[0] ? meta : "You";
            int         mw   = text_w(ml, TXT_TINY);
            add_label(lst, bx + bubble_w - pad_x - mw, bubble_y + pad_y + nl * line_h, TXT_TINY, mcol, ml);
        } else {
            int bx = margin;
            add_rect(lst, bx, bubble_y, bubble_w, bubble_h, COL_HEADER);
            add_rect(lst, bx, bubble_y, 3, bubble_h, COL_ACCENT);
            for (int k = 0; k < nl; k++) {
                emoji_text(lst, bx + pad_x, bubble_y + pad_y + k * line_h, TXT_BODY, COL_WHITE, lines[k]);
            }
            if (meta[0]) {
                add_label(lst, bx + pad_x, bubble_y + pad_y + nl * line_h, TXT_TINY, COL_GRAY, meta);
            }
        }
    }
}

// ── VIEW_CHAT (DM inbox + conversation) ──────────────────────────────────────
// Pixel-matched port of render_chat.c. The emoji-picker overlay reachable while
// typing stays on the PAX path (lvgl_view_active reports false while it's up).

static void render_chat_lvgl(void) {
    int       w   = (int)lvgl_port_width();
    int       h   = (int)lvgl_port_height();
    lv_obj_t* scr = begin_screen(COL_BG);
    pt_reset();
    tab_bar_lvgl(scr);

    if (dm_inbox_mode) {
        int inbox_y0 = TAB_BAR_H + 6;
        int footer_h = 36;
        int inbox_h  = h - inbox_y0 - footer_h;
        int row_h    = 56;
        int rows_vis = inbox_h / row_h;
        if (rows_vis < 1) rows_vis = 1;

        int  idx_map[MAX_CONTACTS + 1];
        int  idx_count     = 0;
        bool active_on_top = dm_target_set;
        if (active_on_top) idx_map[idx_count++] = -1;
        if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            for (int i = 0; i < contact_count && idx_count < MAX_CONTACTS + 1; i++) {
                if (active_on_top && memcmp(contacts[i].pub_key, dm_target_pub, MESHCORE_PUB_KEY_SIZE) == 0) continue;
                idx_map[idx_count++] = i;
            }
            xSemaphoreGive(node_mutex);
        }

        if (idx_count == 0) {
            add_label(scr, 16, inbox_y0 + 18, TXT_BODY, COL_AMBER, "No conversations yet");
            add_label(scr, 16, inbox_y0 + 18 + TXT_BODY + 6, TXT_SMALL, COL_GRAY,
                      "Open the Nodes tab and press Enter on a contact.");
        } else {
            if (dm_inbox_cursor >= idx_count) dm_inbox_cursor = idx_count - 1;
            if (dm_inbox_cursor < 0) dm_inbox_cursor = 0;
            if (dm_inbox_cursor < dm_inbox_scroll) dm_inbox_scroll = dm_inbox_cursor;
            if (dm_inbox_cursor >= dm_inbox_scroll + rows_vis) dm_inbox_scroll = dm_inbox_cursor - rows_vis + 1;
            int max_scroll = idx_count - rows_vis;
            if (max_scroll < 0) max_scroll = 0;
            if (dm_inbox_scroll > max_scroll) dm_inbox_scroll = max_scroll;
            if (dm_inbox_scroll < 0) dm_inbox_scroll = 0;

            for (int row = 0; row < rows_vis; row++) {
                int li = row + dm_inbox_scroll;
                if (li >= idx_count) break;
                int                    e         = idx_map[li];
                bool                   is_active = (e == -1);
                bool                   is_cursor = (li == dm_inbox_cursor);
                const char*            name;
                meshcore_device_role_t role;
                if (is_active) {
                    name   = dm_target_name;
                    int ci = contact_find(dm_target_pub);
                    role   = (ci >= 0) ? (meshcore_device_role_t)contacts[ci].role : MESHCORE_DEVICE_ROLE_CHAT_NODE;
                } else {
                    name = contacts[e].alias;
                    role = (meshcore_device_role_t)contacts[e].role;
                }

                int row_slot   = is_active ? contact_find(dm_target_pub) : e;
                int row_unread = (row_slot >= 0) ? contact_unread[row_slot] : 0;

                int y = inbox_y0 + row * row_h;
                if (is_cursor) {
                    add_rect(scr, 0, y, w, row_h - 2, COL_PANEL);
                    add_rect(scr, 0, y, 5, row_h - 2, COL_ACCENT);
                } else if (row_unread > 0) {
                    add_rect(scr, 0, y, 3, row_h - 2, COL_RED);
                }
                add_rect(scr, 12, y + row_h - 1, w - 24, 1, COL_PANEL);

                int      av_x = 18, av_y = y + (row_h - 36) / 2, av_d = 36;
                uint32_t av_bg = is_active ? COL_AMBER : COL_BLUE;
                add_rect(scr, av_x, av_y, av_d, av_d, av_bg);
                char init[2] = {(char)(name[0] ? toupper((unsigned char)name[0]) : '?'), 0};
                add_label(scr, av_x + (av_d - text_w(init, TXT_TITLE)) / 2, av_y + (av_d - TXT_TITLE) / 2 - 1,
                          TXT_TITLE, COL_HEADER, init);

                int name_x = av_x + av_d + 12;
                add_label(scr, name_x, y + 6, TXT_BODY, COL_WHITE, name);

                if (row_unread > 0) {
                    char ub[8];
                    snprintf(ub, sizeof(ub), "%d", row_unread > 99 ? 99 : row_unread);
                    int nw = text_w(name, TXT_BODY);
                    int bw = text_w(ub, TXT_SMALL) + 12;
                    int bx = name_x + nw + 8;
                    int by = y + 6;
                    add_rect(scr, bx, by, bw, TXT_SMALL + 4, COL_RED);
                    add_label(scr, bx + 6, by + 2, TXT_SMALL, COL_HEADER, ub);
                }

                const char* rl = role_label(role);
                char        sub[64];
                if (row_unread > 0) {
                    snprintf(sub, sizeof(sub), "%s  -  %d new", rl, row_unread > 99 ? 99 : row_unread);
                } else if (is_active) {
                    snprintf(sub, sizeof(sub), "%s  -  active DM", rl);
                } else {
                    snprintf(sub, sizeof(sub), "%s  -  saved contact", rl);
                }
                uint32_t sub_col = (row_unread > 0) ? COL_RED : COL_GRAY;
                add_label(scr, av_x + av_d + 12, y + 6 + TXT_BODY + 4, TXT_SMALL, sub_col, sub);

                if (is_cursor) {
                    const char* cta = "Enter >";
                    add_label(scr, w - text_w(cta, TXT_SMALL) - 12, y + (row_h - TXT_SMALL) / 2, TXT_SMALL, COL_AMBER,
                              cta);
                }
            }

            if (idx_count > rows_vis) {
                char sc2[24];
                snprintf(sc2, sizeof(sc2), "%d/%d", dm_inbox_cursor + 1, idx_count);
                add_label(scr, w - text_w(sc2, TXT_SMALL) - 10, h - footer_h - TXT_SMALL - 2, TXT_SMALL, COL_GRAY, sc2);
            }
        }

        int fy_base = h - footer_h;
        add_rect(scr, 0, fy_base, w, footer_h, COL_HEADER);
        add_rect(scr, 0, fy_base, w, 1, COL_PANEL);
        const char* inbox_hint = "W/S: nav   Enter: open   D: delete   Tab: next   ";
        int         ih_ty      = fy_base + (footer_h - TXT_SMALL) / 2;
        add_label(scr, 10, ih_ty, TXT_SMALL, COL_HINT, inbox_hint);
        add_back_hint(scr, 10 + text_w(inbox_hint, TXT_SMALL), ih_ty, ": home", TXT_SMALL);
        return;
    }

    int input_y = h - CHAT_INPUT_H - FOOTER_H;
    int list_y0 = CHAT_Y0 + 32;
    int list_h  = input_y - list_y0;
    add_rect(scr, 0, CHAT_Y0, w, 28, COL_PANEL);
    {
        char hdr[MESHCORE_MAX_NAME_SIZE + 24];
        snprintf(hdr, sizeof(hdr), "<  %s", dm_target_set ? dm_target_name : "(no target)");
        add_label(scr, 10, CHAT_Y0 + 4, TXT_BODY, COL_WHITE, hdr);
    }

    if (xSemaphoreTake(chat_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        render_msg_list_lvgl(scr, w, list_y0, list_h, chat_msgs, chat_head, chat_count, &chat_scroll, false);
        xSemaphoreGive(chat_mutex);
    }

    int iy = input_y;
    add_rect(scr, 0, iy, w, CHAT_INPUT_H, COL_PANEL);
    add_rect(scr, 0, iy, w, 2, chat_typing ? COL_ACCENT : COL_AMBER);
    if (chat_typing) {
        char prefix[MESHCORE_MAX_NAME_SIZE + 8];
        snprintf(prefix, sizeof(prefix), "DM %s> ", dm_target_name);
        int ty = iy + (CHAT_INPUT_H - TXT_BODY) / 2;
        int pw = emoji_text(scr, 10, ty, TXT_BODY, COL_WHITE, prefix);
        int bw = emoji_text(scr, 10 + pw, ty, TXT_BODY, COL_WHITE, chat_input);
        add_label(scr, 10 + pw + bw, ty, TXT_BODY, COL_WHITE, "_");

        char ctr[12];
        snprintf(ctr, sizeof(ctr), "%d/%d", chat_input_len, MAX_INPUT_LEN);
        add_label(scr, w - text_w(ctr, TXT_SMALL) - 10, iy + (CHAT_INPUT_H - TXT_SMALL) / 2, TXT_SMALL, COL_GRAY, ctr);
    } else {
        add_label(scr, 10, iy + (CHAT_INPUT_H - TXT_SMALL) / 2, TXT_SMALL, COL_AMBER, "T: type message");
    }

    int fy = h - FOOTER_H;
    add_rect(scr, 0, fy, w, FOOTER_H, COL_HEADER);
    add_rect(scr, 0, fy, w, 1, COL_PANEL);
    int hint_ty = fy + (FOOTER_H - TXT_SMALL) / 2;
    if (chat_typing) {
        const char* hint = "Enter: send   Backspace: delete   ";
        add_label(scr, 10, hint_ty, TXT_SMALL, COL_HINT, hint);
        int hx = 10 + text_w(hint, TXT_SMALL);
        add_back_hint(scr, hx, hint_ty, ": cancel   ", TXT_SMALL);
        int xg     = TXT_SMALL / 2 - 1;
        int icon_x = hx + 2 * xg + 4 + text_w(": cancel   ", TXT_SMALL);
        int icon_y = fy + FOOTER_H / 2;
        add_circle(scr, icon_x + 6, icon_y, 6, -1, COL_GREEN, 2);
        add_label(scr, icon_x + 18, hint_ty, TXT_SMALL, COL_HINT, ": emoji");
    } else {
        const char* hint = "T: type   W/S: scroll   Tab: next tab   ";
        add_label(scr, 10, hint_ty, TXT_SMALL, COL_HINT, hint);
        add_back_hint(scr, 10 + text_w(hint, TXT_SMALL), hint_ty, ": back to inbox", TXT_SMALL);
    }

    if (emoji_picker_active && chat_typing) {
        render_emoji_picker_overlay_lvgl(scr, w, h);
    }
}

// ── VIEW_CHANNEL (channel list + wizard + conversation) ──────────────────────
// Pixel-matched port of render_channel.c. The QR (share) overlay and the
// emoji-picker overlay stay on the PAX path (lvgl_view_active reports false
// while either is up).

static void render_channel_list_lvgl(lv_obj_t* scr, int w, int h) {
    const int row_h    = 38;
    const int footer_h = FOOTER_H;

    add_rect(scr, 0, CHAT_Y0, w, 28, COL_PANEL);
    add_label(scr, 10, CHAT_Y0 + 4, TXT_BODY, COL_WHITE, "Channels");

    int rows_y0  = CHAT_Y0 + 32;
    int rows_h   = h - rows_y0 - footer_h;
    int rows_vis = rows_h / row_h;
    if (rows_vis < 1) rows_vis = 1;

    if (channel_list_cursor < 0) channel_list_cursor = 0;
    if (channel_list_cursor >= channel_count) channel_list_cursor = channel_count - 1;

    int scroll = 0;
    if (channel_list_cursor >= rows_vis) scroll = channel_list_cursor - rows_vis + 1;

    for (int row = 0; row < rows_vis && (row + scroll) < channel_count; row++) {
        int  i         = row + scroll;
        int  y         = rows_y0 + row * row_h;
        bool is_sel    = (i == channel_list_cursor);
        bool is_active = (i == active_channel_idx);

        if (is_sel) {
            add_rect(scr, 0, y, w, row_h - 1, COL_PANEL);
            add_rect(scr, 0, y, 5, row_h - 1, COL_ACCENT);
        }
        uint32_t name_col = is_sel ? COL_WHITE : COL_GRAY;
        int      text_y   = y + (row_h - TXT_BODY) / 2;

        if (is_active) {
            add_label(scr, 18, text_y, TXT_BODY, COL_GREEN, ">");
        }
        add_label(scr, 40, text_y, TXT_BODY, name_col, channels[i].name);

        if (channel_unread[i] > 0) {
            char ub[8];
            snprintf(ub, sizeof(ub), "%d", channel_unread[i] > 99 ? 99 : channel_unread[i]);
            int nw = text_w(channels[i].name, TXT_BODY);
            int bw = text_w(ub, TXT_SMALL) + 12;
            int bx = 40 + nw + 8;
            int by = y + (row_h - (TXT_SMALL + 4)) / 2;
            add_rect(scr, bx, by, bw, TXT_SMALL + 4, COL_RED);
            add_label(scr, bx + 6, by + 2, TXT_SMALL, COL_HEADER, ub);
        }

        char meta[24];
        snprintf(meta, sizeof(meta), "0x%02X", channels[i].hash);
        add_label(scr, w - text_w(meta, TXT_BODY) - 14, y + (row_h - TXT_BODY) / 2, TXT_BODY, name_col, meta);
    }

    if (channel_adding && channel_wiz_step == 0) {
        const char* opts[2] = {"# community channel", "private channel"};
        const char* mtitle  = channel_creating ? "Create channel" : "Add channel";
        const int   mpw     = 320;
        const int   mph     = 34 + 2 * 40 + 12;
        int         mpx     = (w - mpw) / 2;
        int         mpy     = (h - mph) / 2;
        add_rect(scr, mpx, mpy, mpw, mph, COL_HEADER);
        add_rect(scr, mpx, mpy, mpw, 2, COL_ACCENT);
        add_label(scr, mpx + 14, mpy + 8, TXT_SMALL, COL_AMBER, mtitle);
        for (int i = 0; i < 2; i++) {
            int oy = mpy + 34 + i * 40;
            if (i == channel_wiz_cursor) {
                add_rect(scr, mpx + 6, oy - 4, mpw - 12, 34, COL_PANEL);
                add_rect(scr, mpx + 6, oy - 4, 4, 34, COL_ACCENT);
            }
            add_label(scr, mpx + 18, oy, TXT_BODY, COL_WHITE, opts[i]);
        }
    } else if (channel_adding) {
        int iy = h - CHAT_INPUT_H - footer_h;
        add_rect(scr, 0, iy, w, CHAT_INPUT_H, COL_PANEL);
        add_rect(scr, 0, iy, w, 2, COL_ACCENT);
        const char* prefix = (channel_wiz_step == 2) ? "secret: " : "name: ";
        const char* shown  = field_edit_buf;
        const int   window = 36;
        if (field_edit_len > window) shown = field_edit_buf + (field_edit_len - window);
        char disp[160];
        snprintf(disp, sizeof(disp), "%s%s_", prefix, shown);
        add_label(scr, 10, iy + (CHAT_INPUT_H - TXT_BODY) / 2, TXT_BODY, COL_WHITE, disp);
    }

    int fy = h - footer_h;
    add_rect(scr, 0, fy, w, footer_h, COL_HEADER);
    add_rect(scr, 0, fy, w, 1, COL_PANEL);
    const char* hint;
    if (channel_adding) {
        if (channel_wiz_step == 0) {
            hint = "W/S: choose   Enter: select   ";
        } else if (channel_wiz_step == 2) {
            hint = "Type the 32-hex secret key   Enter: add   ";
        } else if (channel_wiz_private && !channel_creating) {
            hint = "Type the channel name   Enter: next (secret)   ";
        } else {
            hint = channel_creating ? "Type the channel name   Enter: create   "
                                    : "Type the # channel name (no #)   Enter: add   ";
        }
    } else if (channel_list_cursor == 0) {
        hint = "W/S: nav  Enter: open  A: add  C: create  Q: share  Tab: next  ";
    } else {
        hint = "W/S: nav  Enter: open  A: add  C: create  Q: share  D: del  Tab: next  ";
    }
    int hint_ty = fy + (footer_h - TXT_SMALL) / 2;
    add_label(scr, 10, hint_ty, TXT_SMALL, COL_HINT, hint);
    add_back_hint(scr, 10 + text_w(hint, TXT_SMALL), hint_ty, channel_adding ? ": cancel" : ": home", TXT_SMALL);
}

static void render_channel_lvgl(void) {
    int       w   = (int)lvgl_port_width();
    int       h   = (int)lvgl_port_height();
    lv_obj_t* scr = begin_screen(COL_BG);
    pt_reset();
    tab_bar_lvgl(scr);

    if (channel_list_mode) {
        render_channel_list_lvgl(scr, w, h);
        return;
    }

    const int hdr_h = 50;
    add_rect(scr, 0, CHAT_Y0, w, hdr_h, COL_PANEL);
    {
        const char* nm = (active_channel_idx >= 0 && active_channel_idx < channel_count)
                             ? channels[active_channel_idx].name
                             : "(no channel)";
        add_label(scr, 12, CHAT_Y0 + 4, TXT_BODY, COL_WHITE, nm);

        char sub[48];
        if (region_scope[0]) {
            snprintf(sub, sizeof(sub), "  Region: %s", region_scope);
        } else {
            snprintf(sub, sizeof(sub), "  Region: (set in Settings)");
        }
        uint32_t sub_col = region_scope[0] ? COL_GRAY : COL_AMBER;
        add_label(scr, 12, CHAT_Y0 + 4 + TXT_BODY + 2, TXT_SMALL, sub_col, sub);
    }

    int input_y = h - CHAT_INPUT_H - FOOTER_H;
    int list_y0 = CHAT_Y0 + hdr_h + 4;
    int list_h  = input_y - list_y0;
    if (xSemaphoreTake(ch_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        render_msg_list_lvgl(scr, w, list_y0, list_h, ch_msgs, ch_head, ch_count, &ch_scroll, true);
        xSemaphoreGive(ch_mutex);
    }

    int iy = input_y;
    add_rect(scr, 0, iy, w, CHAT_INPUT_H, COL_PANEL);
    add_rect(scr, 0, iy, w, 2, chat_typing ? COL_ACCENT : COL_GREEN);
    if (chat_typing) {
        int ty = iy + (CHAT_INPUT_H - TXT_BODY) / 2;
        int pw = emoji_text(scr, 10, ty, TXT_BODY, COL_WHITE, "> ");
        int bw = emoji_text(scr, 10 + pw, ty, TXT_BODY, COL_WHITE, chat_input);
        add_label(scr, 10 + pw + bw, ty, TXT_BODY, COL_WHITE, "_");

        char ctr[12];
        snprintf(ctr, sizeof(ctr), "%d/%d", chat_input_len, MAX_INPUT_LEN);
        add_label(scr, w - text_w(ctr, TXT_SMALL) - 10, iy + (CHAT_INPUT_H - TXT_SMALL) / 2, TXT_SMALL, COL_GRAY, ctr);
    } else {
        add_label(scr, 10, iy + (CHAT_INPUT_H - TXT_SMALL) / 2, TXT_SMALL, COL_GREEN, "T: send channel message");
    }

    int fy = h - FOOTER_H;
    add_rect(scr, 0, fy, w, FOOTER_H, COL_HEADER);
    add_rect(scr, 0, fy, w, 1, COL_PANEL);
    int hint_ty = fy + (FOOTER_H - TXT_SMALL) / 2;
    if (chat_typing) {
        const char* hint = "Enter: send   Backspace: delete   ";
        add_label(scr, 10, hint_ty, TXT_SMALL, COL_HINT, hint);
        int hx = 10 + text_w(hint, TXT_SMALL);
        add_back_hint(scr, hx, hint_ty, ": cancel   ", TXT_SMALL);
        int xg     = TXT_SMALL / 2 - 1;
        int icon_x = hx + 2 * xg + 4 + text_w(": cancel   ", TXT_SMALL);
        int icon_y = fy + FOOTER_H / 2;
        add_circle(scr, icon_x + 6, icon_y, 6, -1, COL_GREEN, 2);
        add_label(scr, icon_x + 18, hint_ty, TXT_SMALL, COL_HINT, ": emoji");
    } else {
        const char* hint = "T: type   W/S: scroll   R: clear   Tab: next   ";
        add_label(scr, 10, hint_ty, TXT_SMALL, COL_HINT, hint);
        add_back_hint(scr, 10 + text_w(hint, TXT_SMALL), hint_ty, ": list", TXT_SMALL);
    }

    if (emoji_picker_active && chat_typing) {
        render_emoji_picker_overlay_lvgl(scr, w, h);
    }
}

// ── VIEW_SETTINGS ────────────────────────────────────────────────────────────
// Pixel-matched port of render_settings.c (category-list grid + drilldown) and
// the 9 category glyphs from render_settings_icons.c. The PAX files stay until
// Phase 4; this view reuses the field registry (settings_field_label/value) and
// the category table helpers so no per-field logic is duplicated here.

extern bool c6_available;

#define S_GRID_COLS   4
#define S_GRID_H_MARG 30
#define S_GRID_V_MARG 20
#define S_GRID_FOOTER 38

// PAX 0 rad = +x, +y down; LVGL arc 0 deg = 3 o'clock, increasing clockwise.
// Both run clockwise in screen space, so a plain rad->deg conversion keeps the
// same start/end ordering. (See render_settings_icons.c pax_outline_arc calls.)
static void add_arc_rad(lv_obj_t* p, int cx, int cy, int r, float a0, float a1, int w, uint32_t col) {
    add_arc(p, cx, cy, r, (int)lroundf(a0 * 180.0f / 3.14159265f), (int)lroundf(a1 * 180.0f / 3.14159265f), w, col);
}

// ── Category glyphs (port of render_settings_icons.c) ───────────────────────
// pax_simple_line          -> add_line(...,2,col)
// pax_outline_circle       -> add_circle(...,-1,col,2)   (border only)
// pax_simple_circle        -> add_circle(...,col,-1,0)   (filled)
// pax_outline_hollow_circle-> add_circle(...,outer,-1,col,2) (stroked ring)
// pax_outline_arc          -> add_arc_rad (rad->deg)

static void cat_icon_identity_lv(lv_obj_t* s, int cx, int cy, int sz, uint32_t col) {
    int half = sz / 2;
    int x0 = cx - half, x1 = cx + half;
    int y0 = cy - sz / 3, y1 = cy + sz / 3;
    add_line(s, x0, y0, x1, y0, 2, col);
    add_line(s, x1, y0, x1, y1, 2, col);
    add_line(s, x1, y1, x0, y1, 2, col);
    add_line(s, x0, y1, x0, y0, 2, col);
    int pcx   = cx - sz / 4;
    int headR = sz / 10;
    int headY = cy - sz / 10;
    add_circle(s, pcx, headY, headR, -1, col, 2);
    add_arc_rad(s, pcx, headY + headR + sz / 16, sz / 8, -3.14159265f, 0.0f, 2, col);
    int lx0 = cx + sz / 16, lx1 = x1 - sz / 10;
    int ly = cy - sz / 8;
    for (int i = 0; i < 3; i++) {
        add_line(s, lx0, ly, lx1, ly, 2, col);
        ly += sz / 8;
    }
}

static void cat_icon_regulatory_lv(lv_obj_t* s, int cx, int cy, int sz, uint32_t col) {
    int t = sz / 2;
    add_line(s, cx, cy - t, cx + t, cy - t / 3, 2, col);
    add_line(s, cx + t, cy - t / 3, cx + t, cy + t / 3, 2, col);
    add_line(s, cx + t, cy + t / 3, cx, cy + t, 2, col);
    add_line(s, cx, cy + t, cx - t, cy + t / 3, 2, col);
    add_line(s, cx - t, cy + t / 3, cx - t, cy - t / 3, 2, col);
    add_line(s, cx - t, cy - t / 3, cx, cy - t, 2, col);
}

static void cat_icon_radio_lv(lv_obj_t* s, int cx, int cy, int sz, uint32_t col) {
    int half = sz / 2;
    int x0 = cx - half, x1 = cx + half;
    int y0 = cy - sz / 6, y1 = cy + sz / 3;
    add_line(s, x0 + sz / 6, y0, cx + sz / 5, y0 - sz / 3, 2, col);
    add_line(s, x0, y0, x1, y0, 2, col);
    add_line(s, x1, y0, x1, y1, 2, col);
    add_line(s, x1, y1, x0, y1, 2, col);
    add_line(s, x0, y1, x0, y0, 2, col);
    int bodyH = y1 - y0;
    int spR   = bodyH / 2 - sz / 16;
    int spcx  = x0 + bodyH / 2 + sz / 16;
    int spcy  = (y0 + y1) / 2;
    add_circle(s, spcx, spcy, spR, -1, col, 2);
    int gx0 = spcx + spR + sz / 12, gx1 = x1 - sz / 10;
    int gy = y0 + bodyH / 4;
    for (int i = 0; i < 3; i++) {
        add_line(s, gx0, gy, gx1, gy, 2, col);
        gy += bodyH / 4;
    }
}

static void cat_icon_advert_lv(lv_obj_t* s, int cx, int cy, int sz, uint32_t col) {
    int half = sz / 2;
    int top  = cy - half;
    int base = cy + half * 3 / 4;
    add_line(s, cx, top, cx, base, 2, col);
    add_line(s, cx, base, cx - half / 2, base + half / 4, 2, col);
    add_line(s, cx, base, cx + half / 2, base + half / 4, 2, col);
    add_line(s, cx - half / 3, top + half / 8, cx + half / 3, top + half / 8, 2, col);
    add_circle(s, cx + half / 6, top + half / 6, half / 3, -1, col, 2);
    add_circle(s, cx + half / 6, top + half / 6, half / 2, -1, col, 2);
}

static void cat_icon_network_lv(lv_obj_t* s, int cx, int cy, int sz, uint32_t col) {
    int t = sz / 3;
    add_circle(s, cx, cy - t, sz / 12, col, -1, 0);
    add_circle(s, cx - t, cy + t, sz / 12, col, -1, 0);
    add_circle(s, cx + t, cy + t, sz / 12, col, -1, 0);
    add_line(s, cx, cy - t, cx - t, cy + t, 2, col);
    add_line(s, cx, cy - t, cx + t, cy + t, 2, col);
    add_line(s, cx - t, cy + t, cx + t, cy + t, 2, col);
}

static void cat_icon_region_lv(lv_obj_t* s, int cx, int cy, int sz, uint32_t col) {
    int r = sz / 3;
    add_circle(s, cx, cy - r / 2, r, -1, col, 2);
    add_line(s, cx - r * 7 / 10, cy - r / 8, cx, cy + r * 5 / 4, 2, col);
    add_line(s, cx + r * 7 / 10, cy - r / 8, cx, cy + r * 5 / 4, 2, col);
    add_circle(s, cx, cy - r / 2, r / 3, col, -1, 0);
}

static void cat_icon_brightness_lv(lv_obj_t* s, int cx, int cy, int sz, uint32_t col) {
    int r = sz / 5;
    add_circle(s, cx, cy, r, col, -1, 0);
    for (int a = 0; a < 8; a++) {
        float th = (float)a * 3.14159f / 4.0f;
        float r0 = (float)r + sz / 14.0f;
        float r1 = (float)sz / 2.0f;
        add_line(s, cx + (int)lroundf(r0 * cosf(th)), cy + (int)lroundf(r0 * sinf(th)),
                 cx + (int)lroundf(r1 * cosf(th)), cy + (int)lroundf(r1 * sinf(th)), 2, col);
    }
}

static void cat_icon_sounds_lv(lv_obj_t* s, int cx, int cy, int sz, uint32_t col) {
    int half = sz / 2;
    int t    = half * 2 / 3;
    add_line(s, cx - t / 2, cy - t / 4, cx - t / 2, cy + t / 4, 2, col);
    add_line(s, cx - t / 2, cy - t / 4, cx, cy - t / 2, 2, col);
    add_line(s, cx, cy - t / 2, cx, cy + t / 2, 2, col);
    add_line(s, cx, cy + t / 2, cx - t / 2, cy + t / 4, 2, col);
    float pi = 3.14159265f;
    add_arc_rad(s, cx, cy, half * 5 / 10, -pi / 4.0f, pi / 4.0f, 2, col);
    add_arc_rad(s, cx, cy, half * 8 / 10, -pi / 4.0f, pi / 4.0f, 2, col);
}

static void cat_icon_toolbox_lv(lv_obj_t* s, int cx, int cy, int sz, uint32_t col) {
    int half = sz / 2;
    int bx0 = cx - half, bx1 = cx + half;
    int by0 = cy - half / 5, by1 = cy + half;
    add_line(s, bx0, by0, bx1, by0, 2, col);
    add_line(s, bx1, by0, bx1, by1, 2, col);
    add_line(s, bx1, by1, bx0, by1, 2, col);
    add_line(s, bx0, by1, bx0, by0, 2, col);
    add_line(s, bx0, cy + half / 4, bx1, cy + half / 4, 2, col);
    int hx0 = cx - half / 3, hx1 = cx + half / 3, hy = cy - half / 2;
    add_line(s, hx0, by0, hx0, hy, 2, col);
    add_line(s, hx1, by0, hx1, hy, 2, col);
    add_line(s, hx0, hy, hx1, hy, 2, col);
}

typedef void (*cat_icon_lv_fn)(lv_obj_t*, int, int, int, uint32_t);
// Index order MUST match s_categories[] in render_settings.c (and the PAX
// settings_category_icons[] table).
static const cat_icon_lv_fn s_cat_icons_lv[] = {
    cat_icon_identity_lv, cat_icon_regulatory_lv, cat_icon_radio_lv,  cat_icon_advert_lv,  cat_icon_network_lv,
    cat_icon_region_lv,   cat_icon_brightness_lv, cat_icon_sounds_lv, cat_icon_toolbox_lv,
};
#define S_CAT_ICONS_LV_N ((int)(sizeof(s_cat_icons_lv) / sizeof(s_cat_icons_lv[0])))

static void render_settings_category_list_lvgl(lv_obj_t* scr, int w, int h) {
    int area_y0 = TAB_BAR_H + S_GRID_V_MARG;
    int area_h  = h - area_y0 - S_GRID_V_MARG - S_GRID_FOOTER;
    int area_w  = w - S_GRID_H_MARG * 2;

    int visible = settings_visible_category_count();
    int rows    = (visible + S_GRID_COLS - 1) / S_GRID_COLS;
    if (rows < 1) rows = 1;

    int tile_w = (area_w - S_GRID_H_MARG * (S_GRID_COLS - 1)) / S_GRID_COLS;
    int tile_h = (area_h - S_GRID_V_MARG * (rows - 1)) / rows;

    for (int slot = 0; slot < visible; slot++) {
        int i = settings_visible_category_real_idx(slot);
        if (i < 0) continue;
        int col = slot % S_GRID_COLS;
        int row = slot / S_GRID_COLS;
        int tx  = S_GRID_H_MARG + col * (tile_w + S_GRID_H_MARG);
        int ty  = area_y0 + row * (tile_h + S_GRID_V_MARG);

        bool     focused = (slot == settings_category_cursor);
        uint32_t bg      = focused ? COL_PAGER_ACCENT : COL_PAGER_TILE;
        uint32_t fg      = focused ? COL_HEADER : COL_PAGER_TEXT;

        add_rect(scr, tx, ty, tile_w, tile_h, bg);
        if (focused) {
            add_rect(scr, tx + 2, ty + 2, tile_w - 4, 2, COL_PAGER_BG);
            add_rect(scr, tx + 2, ty + tile_h - 4, tile_w - 4, 2, COL_PAGER_BG);
            add_rect(scr, tx + 2, ty + 2, 2, tile_h - 4, COL_PAGER_BG);
            add_rect(scr, tx + tile_w - 4, ty + 2, 2, tile_h - 4, COL_PAGER_BG);
        }

        int icon_sz = tile_w / 2;
        if (icon_sz > tile_h * 2 / 5) icon_sz = tile_h * 2 / 5;
        int icon_cx = tx + tile_w / 2;
        int icon_cy = ty + tile_h * 2 / 5;
        if (i < S_CAT_ICONS_LV_N && s_cat_icons_lv[i]) {
            s_cat_icons_lv[i](scr, icon_cx, icon_cy, icon_sz, fg);
        }

        const char* lbl     = settings_category_title(i);
        const char* nl      = strchr(lbl, '\n');
        int         ly_base = ty + tile_h * 3 / 4;
        if (nl) {
            char line1[40];
            int  n1 = (int)(nl - lbl);
            if (n1 >= (int)sizeof(line1)) n1 = sizeof(line1) - 1;
            memcpy(line1, lbl, n1);
            line1[n1]         = '\0';
            const char* line2 = nl + 1;
            int         ly1   = ly_base - (TXT_BODY / 2 + 2);
            int         ly2   = ly_base + (TXT_BODY / 2 + 2);
            add_label(scr, tx + (tile_w - text_w(line1, TXT_BODY)) / 2, ly1, TXT_BODY, fg, line1);
            add_label(scr, tx + (tile_w - text_w(line2, TXT_BODY)) / 2, ly2, TXT_BODY, fg, line2);
        } else {
            add_label(scr, tx + (tile_w - text_w(lbl, TXT_BODY)) / 2, ly_base, TXT_BODY, fg, lbl);
        }
    }

    int fy = h - S_GRID_FOOTER;
    add_rect(scr, 0, fy, w, S_GRID_FOOTER, COL_HEADER);
    add_rect(scr, 0, fy, w, 1, COL_PAGER_ACCENT);
    const char* hint    = "WSAD: nav   Enter: open   Tab: next view   ";
    int         hint_ty = fy + (S_GRID_FOOTER - TXT_SMALL) / 2;
    add_label(scr, 10, hint_ty, TXT_SMALL, COL_HINT, hint);
    add_back_hint(scr, 10 + text_w(hint, TXT_SMALL), hint_ty, ": home", TXT_SMALL);
}

static void render_settings_drilldown_lvgl(lv_obj_t* scr, int w, int h) {
    int first_field, last_field;
    settings_category_bounds(settings_category_active, &first_field, &last_field);

    if (edit_mode) {
        const char* mode_str = "[EDIT]";
        add_label(scr, w - text_w(mode_str, TXT_SMALL) - 110, (TAB_BAR_H - TXT_SMALL) / 2, TXT_SMALL, COL_AMBER,
                  mode_str);
    }

    const int row_h    = 44;
    const int sec_h    = 26;
    const int title_h  = 38;
    const int footer_h = 60;
    const int y0       = TAB_BAR_H + 6;

    // Flatten the 2-line category title ("Region &\nLocation") for the bar.
    {
        const char* src = settings_category_title(settings_category_active);
        char        flat[40];
        size_t      j = 0;
        for (size_t i = 0; src[i] && j + 1 < sizeof(flat); i++) flat[j++] = (src[i] == '\n') ? ' ' : src[i];
        flat[j] = '\0';
        add_label(scr, 18, y0, TXT_TITLE, COL_AMBER, flat);
    }
    add_rect(scr, 18, y0 + TXT_TITLE + 4, w - 36, 1, COL_AMBER);

    int list_y0 = y0 + title_h;
    int list_h  = h - list_y0 - footer_h;

    if (selected < first_field) selected = first_field;
    if (selected > last_field) selected = last_field;

    // Scroll pre-pass (identical math to the PAX renderer) — survives the
    // variable-height inline section headers.
    int field_y[FIELD_COUNT] = {0};
    int total_h = 0, sel_top = 0, sel_bot = 0;
    for (int f = first_field; f <= last_field; f++) {
        if (settings_section_above((field_t)f)) total_h += sec_h;
        field_y[f] = total_h;
        if (f == selected) sel_top = total_h;
        total_h += row_h;
        if (f == selected) sel_bot = total_h;
    }
    if (sel_top < settings_scroll) settings_scroll = sel_top;
    if (sel_bot > settings_scroll + list_h) settings_scroll = sel_bot - list_h;
    int max_scroll = total_h - list_h;
    if (max_scroll < 0) max_scroll = 0;
    if (settings_scroll > max_scroll) settings_scroll = max_scroll;
    if (settings_scroll < 0) settings_scroll = 0;

    int text_y_off = (row_h - TXT_BODY) / 2;

    // Clip container at the list region (LVGL clips children to it, like the
    // PAX pax_clip). Child coordinates below are LOCAL to this container, so the
    // absolute (list_y0 + ...) of the PAX path becomes just (field_y - scroll).
    lv_obj_t* lst = lv_obj_create(scr);
    lv_obj_remove_style_all(lst);
    lv_obj_set_pos(lst, 0, list_y0);
    lv_obj_set_size(lst, w, list_h);
    lv_obj_clear_flag(lst, LV_OBJ_FLAG_SCROLLABLE);

    for (int f = first_field; f <= last_field; f++) {
        int y = field_y[f] - settings_scroll;

        const char* hdr = settings_section_above((field_t)f);
        if (hdr) {
            int hy = y - sec_h;
            if (hy < list_h && hy + sec_h > 0) {
                add_label(lst, 18, hy + (sec_h - TXT_SMALL) / 2 - 1, TXT_SMALL, COL_AMBER, hdr);
                int line_x = 18 + text_w(hdr, TXT_SMALL) + 10;
                int line_y = hy + sec_h - 6;
                add_rect(lst, line_x, line_y, w - line_x - 18, 1, COL_PANEL);
            }
        }

        if (y + row_h <= 0 || y >= list_h) continue;

        bool is_sel = (f == selected);
        if (is_sel) {
            uint32_t bg  = edit_mode ? 0xFF3A2A1A : COL_PANEL;
            uint32_t bar = edit_mode ? COL_AMBER : COL_ACCENT;
            add_rect(lst, 0, y, w, row_h - 1, bg);
            add_rect(lst, 0, y, 5, row_h - 1, bar);
        }
        add_rect(lst, 12, y + row_h - 1, w - 24, 1, COL_PANEL);

        uint32_t lbl_col = is_sel ? COL_WHITE : COL_GRAY;
        add_label(lst, 18, y + text_y_off, TXT_BODY, lbl_col, settings_field_label((field_t)f));

        uint32_t val_col;
        bool     regulatory_violation = false;
        if (f == FIELD_FREQ || f == FIELD_POWER || f == FIELD_COUNTRY) {
            const regulatory_country_t* rc = region_get_country(country_code);
            if (rc && rc->n_subbands > 0) {
                const regulatory_subband_t* sb = region_match_subband(rc, (float)lora_cfg.frequency / 1000000.0f);
                if (!sb) {
                    regulatory_violation = true;
                } else if (f == FIELD_POWER) {
                    int8_t eff = region_effective_power_dbm(rc, (int8_t)lora_cfg.power, antenna_gain_dbi);
                    if (eff > sb->max_power_dbm) regulatory_violation = true;
                }
            }
        }
        if (f >= FIELD_FREQ && !c6_available) {
            val_col = COL_AMBER;
        } else if (regulatory_violation) {
            val_col = COL_RED;
        } else if (is_sel && edit_mode) {
            val_col = COL_AMBER;
        } else if (is_sel) {
            val_col = COL_WHITE;
        } else {
            val_col = COL_GREEN;
        }

        char value[64];
        settings_field_value((field_t)f, value, sizeof(value));
        char val_disp[80];
        bool is_text_field = (f == FIELD_OWNER || f == FIELD_ADV_NAME || f == FIELD_REGION_SCOPE ||
                              f == FIELD_GPS_LAT || f == FIELD_GPS_LON);
        if (is_sel && edit_mode && field_editing_text && is_text_field) {
            snprintf(val_disp, sizeof(val_disp), "%.76s_", field_edit_buf);
        } else if (is_sel && edit_mode && !is_text_field) {
            snprintf(val_disp, sizeof(val_disp), "< %s >", value);
        } else {
            snprintf(val_disp, sizeof(val_disp), "%s", value);
        }
        add_label(lst, w - text_w(val_disp, TXT_BODY) - 18, y + text_y_off, TXT_BODY, val_col, val_disp);
    }

    int fy = h - footer_h;
    add_rect(scr, 0, fy, w, footer_h, COL_HEADER);
    add_rect(scr, 0, fy, w, 1, COL_PANEL);

    const char* hint = NULL;
    const char* back = ": back to categories";
    char        hintbuf[128];
    uint32_t    hint_col = COL_HINT;
    if (edit_mode && field_editing_text) {
        hint = "Type to edit   Backspace: del   Enter: save   ";
        back = ": cancel";
    } else if (edit_mode) {
        hint = "Up/Down or W/S: adjust   Enter: save   ";
        back = ": cancel";
    } else if (!c6_available) {
        hint     = "NVS only -- update radio via Launcher: Tools > Firmware update";
        hint_col = COL_AMBER;
    } else if (selected == FIELD_ANTENNA_GAIN) {
        hint = "Antenna gain raises ERP; editable once Country is set.";
    } else if (selected == FIELD_COUNTRY || selected == FIELD_FREQ || selected == FIELD_POWER ||
               selected == FIELD_DUTY_CYCLE) {
        const regulatory_country_t* rc = region_get_country(country_code);
        if (!rc || rc->n_subbands == 0) {
            hint = "Set Country to see allowed band / power / duty-cycle limits.";
        } else {
            const regulatory_subband_t* sb   = region_match_subband(rc, (float)lora_cfg.frequency / 1000000.0f);
            const char*                 unit = (rc->power_unit == POWER_UNIT_EIRP) ? "EIRP" : "ERP";
            if (!sb) {
                snprintf(hintbuf, sizeof(hintbuf), "%s: %.3f MHz off-band -- pick a frequency in an allowed sub-band.",
                         rc->display_name, (double)lora_cfg.frequency / 1e6);
            } else {
                snprintf(hintbuf, sizeof(hintbuf), "%s %s: %.2f-%.2f MHz, max %d dBm %s, %u.%u%% duty cycle.",
                         rc->display_name, sb->label, (double)sb->freq_min_mhz, (double)sb->freq_max_mhz,
                         (int)sb->max_power_dbm, unit, sb->duty_cycle_permille / 10u, sb->duty_cycle_permille % 10u);
            }
            hint = hintbuf;
        }
    } else if (selected == FIELD_OWNER) {
        hint = "Owner name is shared with launcher (Enter to edit)";
    } else if (selected == FIELD_ADV_NAME) {
        hint = "Advert name overrides owner in LoRa adverts (empty=use owner)";
    } else if (selected == FIELD_SYNC) {
        hint = "Sync word: 0x12 = public MeshCore. A different value = separate net.";
    } else if (selected == FIELD_PREAMBLE) {
        hint = "Preamble (default 8): longer = better weak-signal detect, +airtime.";
    } else if (selected == FIELD_FLOOD_ADVERT_INT) {
        hint = "Flood advert interval: 0 = off. Longer = less mesh traffic + battery.";
    } else if (selected == FIELD_DIRECT_ADVERT_INT) {
        hint = "Direct advert interval: 0 = off. Periodic direct send (vs flood)";
    } else if (selected == FIELD_SEND_FLOOD_NOW) {
        hint = "Press OK to emit a single flood advert right now";
    } else if (selected == FIELD_SEND_DIRECT_NOW) {
        hint = "Press OK: direct advert (1-hop, only LoRa neighbours)";
    } else if (selected == FIELD_PRESET) {
        hint = "Preset overwrites SF/BW/CR. MeshCore = default net.";
    } else if (selected == FIELD_ROLE) {
        hint = "Role: advertised only. Does NOT enable repeater behavior.";
    } else if (selected == FIELD_GPS_LAT || selected == FIELD_GPS_LON) {
        hint = "Decimal degrees (e.g. 52.123456). Empty clears both axes.";
    } else if (selected == FIELD_DISPLAY_BL || selected == FIELD_KB_BL || selected == FIELD_LED_BR ||
               selected == FIELD_BLANK_AFTER) {
        // Two-line footer: yellow-square blank/wake icon hint + nav hint.
        hint              = NULL;
        int         top_y = fy + 4;
        int         bot_y = top_y + TXT_BODY + 4;
        const char* pre   = "Press ";
        const char* post  = " to blank / wake display";
        add_label(scr, 10, top_y, TXT_BODY, hint_col, pre);
        int icon_sz = TXT_BODY - 6;
        int icon_x  = 10 + text_w(pre, TXT_BODY);
        int icon_y  = top_y + (TXT_BODY - icon_sz) / 2;
        add_rect(scr, icon_x, icon_y, icon_sz, icon_sz, COL_YELLOW);
        add_label(scr, icon_x + icon_sz + 4, top_y, TXT_BODY, hint_col, post);
        const char* nav = "W/S: navigate   Enter: edit   R: reload   ";
        add_label(scr, 10, bot_y, TXT_SMALL, hint_col, nav);
        add_back_hint(scr, 10 + text_w(nav, TXT_SMALL), bot_y, ": back", TXT_SMALL);
    } else {
        hint = "W/S: navigate   Enter: edit   R: reload   ";
    }
    if (hint) {
        add_label(scr, 10, fy + 6, TXT_BODY, hint_col, hint);
        if (back) {
            add_back_hint(scr, 10, fy + 6 + TXT_BODY + 4, back, TXT_SMALL);
        }
    }

    if (dirty) {
        const char* unsaved = "* unsaved";
        add_label(scr, w - text_w(unsaved, TXT_BODY) - 10, fy + 6, TXT_BODY, COL_AMBER, unsaved);
    }
}

static void render_settings_lvgl(void) {
    int       w   = (int)lvgl_port_width();
    int       h   = (int)lvgl_port_height();
    lv_obj_t* scr = begin_screen(COL_BLACK);
    pt_reset();

    tab_bar_lvgl(scr);

    if (settings_category_list_mode) {
        render_settings_category_list_lvgl(scr, w, h);
    } else {
        render_settings_drilldown_lvgl(scr, w, h);
    }
}

// ── VIEW_MAP ─────────────────────────────────────────────────────────────────
// Port of render_map.c. The OSM tile raster is composited into a persistent
// lv_canvas (its own PSRAM pixel buffer) so the cache tiles can be safely
// evicted by the loader task the instant we release the cache lock — the canvas
// owns a private copy. All other map-area graphics (node pins, crosshair, scale
// bar, legend, status pills, nearest-node panel) are drawn as shapes onto the
// same canvas so PAX's exact draw-order + alpha blending is reproduced; only
// the text, header/footer strips, and the first-fix toast are LVGL objects
// layered on top.

#define MAP_LV_HEADER_H 44
#define MAP_LV_FOOTER_H 26

// Role pin palette — mirrors render_map.c's ROLE_COL_* macros.
#define MAP_PIN_CHAT   COL_GREEN
#define MAP_PIN_RPTR   COL_BLUE
#define MAP_PIN_ROOM   0xFFBB9AF7
#define MAP_PIN_SENSOR COL_AMBER

// Persistent canvas pixel buffer (RGB565, map-area sized). Allocated once in
// PSRAM and reused; realloced only when the map area dimensions change.
static uint8_t* s_map_canvas_buf = NULL;
static int      s_map_canvas_w   = 0;
static int      s_map_canvas_h   = 0;

// First-fix toast latch — one-shot per process; mirrors render_map.c's static.
static bool s_map_first_fix_seen = false;

static uint32_t map_role_pin_color(meshcore_device_role_t r) {
    switch (r) {
        case MESHCORE_DEVICE_ROLE_REPEATER:
            return MAP_PIN_RPTR;
        case MESHCORE_DEVICE_ROLE_ROOM_SERVER:
            return MAP_PIN_ROOM;
        case MESHCORE_DEVICE_ROLE_SENSOR:
            return MAP_PIN_SENSOR;
        case MESHCORE_DEVICE_ROLE_CHAT_NODE:
        default:
            return MAP_PIN_CHAT;
    }
}

// ── Canvas-layer draw primitives (argb: top byte is the opacity) ─────────────
static void cv_rect(lv_layer_t* l, int x, int y, int w, int h, uint32_t argb) {
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color  = mc_col(argb);
    d.bg_opa    = (lv_opa_t)((argb >> 24) & 0xFF);
    lv_area_t a = {x, y, x + w - 1, y + h - 1};
    lv_draw_rect(l, &d, &a);
}

static void cv_circle(lv_layer_t* l, int cx, int cy, int r, uint32_t argb) {
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color  = mc_col(argb);
    d.bg_opa    = (lv_opa_t)((argb >> 24) & 0xFF);
    d.radius    = LV_RADIUS_CIRCLE;
    lv_area_t a = {cx - r, cy - r, cx + r, cy + r};
    lv_draw_rect(l, &d, &a);
}

static void cv_ring(lv_layer_t* l, int cx, int cy, int r, uint32_t argb, int bw) {
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa       = LV_OPA_TRANSP;
    d.radius       = LV_RADIUS_CIRCLE;
    d.border_color = mc_col(argb);
    d.border_opa   = (lv_opa_t)((argb >> 24) & 0xFF);
    d.border_width = bw;
    lv_area_t a    = {cx - r, cy - r, cx + r, cy + r};
    lv_draw_rect(l, &d, &a);
}

static void cv_tri(lv_layer_t* l, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t argb) {
    lv_draw_triangle_dsc_t d;
    lv_draw_triangle_dsc_init(&d);
    d.p[0]  = (lv_point_precise_t){x0, y0};
    d.p[1]  = (lv_point_precise_t){x1, y1};
    d.p[2]  = (lv_point_precise_t){x2, y2};
    d.color = mc_col(argb);
    d.opa   = (lv_opa_t)((argb >> 24) & 0xFF);
    lv_draw_triangle(l, &d);
}

// Filled role-shape pin with a 1 px black halo, centred at canvas-local (cx,cy).
// Shape mapping matches render_map.c::draw_role_pin.
static void cv_pin(lv_layer_t* l, int cx, int cy, meshcore_device_role_t role, bool favorite) {
    uint32_t col = map_role_pin_color(role);
    switch (role) {
        case MESHCORE_DEVICE_ROLE_REPEATER:
            cv_rect(l, cx - 4, cy - 4, 9, 9, 0xFF000000);
            cv_rect(l, cx - 3, cy - 3, 7, 7, col);
            break;
        case MESHCORE_DEVICE_ROLE_ROOM_SERVER:
            cv_tri(l, cx - 5, cy, cx, cy - 5, cx + 5, cy, 0xFF000000);
            cv_tri(l, cx - 5, cy, cx, cy + 5, cx + 5, cy, 0xFF000000);
            cv_tri(l, cx - 4, cy, cx, cy - 4, cx + 4, cy, col);
            cv_tri(l, cx - 4, cy, cx, cy + 4, cx + 4, cy, col);
            break;
        case MESHCORE_DEVICE_ROLE_SENSOR:
            cv_tri(l, cx - 5, cy + 4, cx + 5, cy + 4, cx, cy - 5, 0xFF000000);
            cv_tri(l, cx - 4, cy + 3, cx + 4, cy + 3, cx, cy - 4, col);
            break;
        case MESHCORE_DEVICE_ROLE_CHAT_NODE:
        default:
            cv_circle(l, cx, cy, 5, 0xFF000000);
            cv_circle(l, cx, cy, 4, col);
            break;
    }
    if (favorite) {
        cv_ring(l, cx, cy, 7, COL_WHITE, 1);
    }
}

// (lat, lon) → canvas-local pixel, using the cached centre tile projection.
// Returns false if outside the [0,map_w) × [0,map_h) canvas window.
static bool map_latlon_to_canvas(double lat, double lon, int zoom, int ctx, int cty, int cpx, int cpy, int map_w,
                                 int map_h, int* out_x, int* out_y) {
    int tx, ty, px, py;
    map_latlon_to_tile(lat, lon, zoom, &tx, &ty, &px, &py);
    int dx_px = (tx - ctx) * MAP_TILE_PX + (px - cpx);
    int dy_px = (ty - cty) * MAP_TILE_PX + (py - cpy);
    int lx    = map_w / 2 + dx_px;
    int ly    = map_h / 2 + dy_px;
    if (lx < 0 || lx >= map_w || ly < 0 || ly >= map_h) return false;
    *out_x = lx;
    *out_y = ly;
    return true;
}

// First-fix toast / shared status toast, pixel-matched to render.c::render_toast.
static void map_toast_lvgl(lv_obj_t* scr, int w, int h) {
    if (!toast_text[0]) return;
    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (now_ms - toast_start_ms < toast_duration_ms) {
        int box_w = text_w(toast_text, TXT_TITLE) + 60;
        int box_h = TXT_TITLE + 40;
        int box_x = (w - box_w) / 2;
        int box_y = (h - box_h) / 2;
        add_rect(scr, box_x, box_y, box_w, box_h, COL_PAGER_BG);
        add_rect(scr, box_x, box_y, box_w, 3, COL_PAGER_ACCENT);
        add_rect(scr, box_x, box_y + box_h - 3, box_w, 3, COL_PAGER_ACCENT);
        add_label(scr, box_x + 30, box_y + 20, TXT_TITLE, COL_PAGER_ACCENT, toast_text);
    } else {
        toast_text[0]     = 0;
        toast_duration_ms = 2000;
    }
}

static void render_map_lvgl(void) {
    int       w   = (int)lvgl_port_width();
    int       h   = (int)lvgl_port_height();
    lv_obj_t* scr = begin_screen(COL_BG);
    pt_reset();

    // Lock-to-position: snap the centre to the latest GPS fix before drawing.
    if (map_lock_on && gps_live_valid) {
        map_center_lat_e6 = gps_live_lat_e6;
        map_center_lon_e6 = gps_live_lon_e6;
    }

    int map_y0 = MAP_LV_HEADER_H;
    int map_w  = w;
    int map_h  = h - MAP_LV_HEADER_H - MAP_LV_FOOTER_H;
    if (map_h < 1) map_h = 1;

    double lat = (double)map_center_lat_e6 / 1e6;
    double lon = (double)map_center_lon_e6 / 1e6;
    int    z   = (int)map_zoom;

    // ── Tile raster + map-area overlays on a persistent canvas ───────────────
    if (s_map_canvas_buf == NULL || s_map_canvas_w != map_w || s_map_canvas_h != map_h) {
        if (s_map_canvas_buf) heap_caps_free(s_map_canvas_buf);
        s_map_canvas_buf = heap_caps_malloc((size_t)map_w * map_h * 2, MALLOC_CAP_SPIRAM);
        s_map_canvas_w   = map_w;
        s_map_canvas_h   = map_h;
    }

    if (s_map_canvas_buf) {
        lv_obj_t* canvas = lv_canvas_create(scr);
        lv_obj_set_style_pad_all(canvas, 0, 0);
        lv_obj_clear_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
        lv_canvas_set_buffer(canvas, s_map_canvas_buf, map_w, map_h, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(canvas, 0, map_y0);
        // Missing-tile backdrop (matches render_map.c's COL_PANEL fill).
        lv_canvas_fill_bg(canvas, mc_col(COL_PANEL), LV_OPA_COVER);

        int center_tx, center_ty, px_in, py_in;
        map_latlon_to_tile(lat, lon, z, &center_tx, &center_ty, &px_in, &py_in);
        int origin_x = map_w / 2 - px_in;  // canvas-local
        int origin_y = map_h / 2 - py_in;

        // Layer 1 — tiles. The cache lock is held across lv_canvas_finish_layer
        // because lv_draw_image only *enqueues*; the tile pixels are actually
        // read (copied into the canvas buffer) during finish_layer's dispatch.
        lv_image_dsc_t imgs[25];
        int            img_n = 0;
        lv_layer_t     tlayer;
        lv_canvas_init_layer(canvas, &tlayer);
        map_cache_lock();
        for (int dy = -2; dy <= 2; dy++) {
            for (int dx = -2; dx <= 2; dx++) {
                int        tile_x  = center_tx + dx;
                int        tile_y  = center_ty + dy;
                int        tx      = origin_x + dx * MAP_TILE_PX;
                int        ty      = origin_y + dy * MAP_TILE_PX;
                bool       visible = !(tx + MAP_TILE_PX < 0 || tx >= map_w || ty + MAP_TILE_PX < 0 || ty >= map_h);
                // Request every cell (visible + ring) so a one-step pan finds
                // the new edge tiles already warm in the LRU cache.
                pax_buf_t* t       = map_tile_get(z, tile_x, tile_y);
                if (!visible || !t || img_n >= (int)(sizeof(imgs) / sizeof(imgs[0]))) continue;
                lv_image_dsc_t* img = &imgs[img_n++];
                memset(img, 0, sizeof(*img));
                img->header.magic  = LV_IMAGE_HEADER_MAGIC;
                img->header.cf     = LV_COLOR_FORMAT_RGB565;
                img->header.w      = MAP_TILE_PX;
                img->header.h      = MAP_TILE_PX;
                img->header.stride = MAP_TILE_PX * 2;
                img->data_size     = MAP_TILE_PX * MAP_TILE_PX * 2;
                img->data          = (const uint8_t*)pax_buf_get_pixels(t);
                lv_draw_image_dsc_t id;
                lv_draw_image_dsc_init(&id);
                id.src           = img;
                lv_area_t coords = {tx, ty, tx + MAP_TILE_PX - 1, ty + MAP_TILE_PX - 1};
                lv_draw_image(&tlayer, &id, &coords);
            }
        }
        lv_canvas_finish_layer(canvas, &tlayer);  // tile pixels copied here…
        map_cache_unlock();                       // …so the lock can drop now.

        // Layer 2 — map-area shapes (no cache memory referenced).
        lv_layer_t olayer;
        lv_canvas_init_layer(canvas, &olayer);

        // Node pins + nearest-to-centre name (canvas-local coords).
        int  chx                              = map_w / 2;
        int  chy                              = map_h / 2;
        int  best_d2                          = INT32_MAX;
        int  best_x                           = 0;
        int  best_y                           = 0;
        char best_name[CONTACT_ALIAS_LEN + 1] = {0};
        if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            for (int i = 0; i < node_count; i++) {
                node_entry_t* n = &node_list[i];
                if (!n->position_valid) continue;
                double nlat = (double)n->lat / 1e6;
                double nlon = (double)n->lon / 1e6;
                int    fx, fy;
                if (!map_latlon_to_canvas(nlat, nlon, z, center_tx, center_ty, px_in, py_in, map_w, map_h, &fx, &fy)) {
                    continue;
                }
                bool favorite = contact_find(n->pub_key) >= 0;
                cv_pin(&olayer, fx, fy, n->role, favorite);
                int dx2 = fx - chx;
                int dy2 = fy - chy;
                int d2  = dx2 * dx2 + dy2 * dy2;
                if (d2 < best_d2) {
                    best_d2 = d2;
                    best_x  = fx;
                    best_y  = fy;
                    strncpy(best_name, n->name, CONTACT_ALIAS_LEN);
                    best_name[CONTACT_ALIAS_LEN] = 0;
                }
            }
            xSemaphoreGive(node_mutex);
        }

        // Crosshair (black halo + white core) at the canvas centre.
        int arm = 8;
        cv_rect(&olayer, chx - arm - 1, chy - 1, 2 * arm + 2, 3, 0xFF000000);
        cv_rect(&olayer, chx - 1, chy - arm - 1, 3, 2 * arm + 2, 0xFF000000);
        cv_rect(&olayer, chx - arm, chy, 2 * arm + 1, 1, COL_WHITE);
        cv_rect(&olayer, chx, chy - arm, 1, 2 * arm + 1, COL_WHITE);

        // Scale bar (bar + halo only; label text is an object below).
        char scale_buf[16] = {0};
        int  scale_tx = 0, scale_ty_local = 0, scale_pill_w = 0;
        {
            double lat_rad  = lat * M_PI / 180.0;
            double earth_eq = 40075016.686;
            double m_per_px = (earth_eq * cos(lat_rad)) / ((double)MAP_TILE_PX * (double)(1 << z));
            if (m_per_px > 0) {
                static const int steps[]  = {100, 250, 500, 1000, 2000, 5000, 10000, 25000};
                int              chosen   = steps[0];
                int              chosen_w = (int)(chosen / m_per_px);
                for (size_t i = 1; i < sizeof(steps) / sizeof(steps[0]); i++) {
                    int wpx = (int)(steps[i] / m_per_px);
                    if (abs(wpx - 120) < abs(chosen_w - 120)) {
                        chosen   = steps[i];
                        chosen_w = wpx;
                    }
                }
                int bar_h = 6;
                int bx    = 12;
                int by    = map_h - 28;  // canvas-local
                cv_rect(&olayer, bx - 1, by - 1, chosen_w + 2, bar_h + 2, 0xFF000000);
                cv_rect(&olayer, bx, by, chosen_w, bar_h, COL_WHITE);
                if (chosen >= 1000)
                    snprintf(scale_buf, sizeof(scale_buf), "%d km", chosen / 1000);
                else
                    snprintf(scale_buf, sizeof(scale_buf), "%d m", chosen);
                scale_tx       = bx;
                scale_ty_local = by - TXT_SMALL - 4;
                scale_pill_w   = text_w(scale_buf, TXT_SMALL) + 8;
                cv_rect(&olayer, scale_tx - 4, scale_ty_local - 2, scale_pill_w, TXT_SMALL + 4, 0xC0000000);
            }
        }

        // Legend panel + role-shape swatches (text is objects below).
        static const struct {
            meshcore_device_role_t r;
            const char*            l;
        } legend[] = {
            {MESHCORE_DEVICE_ROLE_CHAT_NODE, "Chat"},
            {MESHCORE_DEVICE_ROLE_REPEATER, "Rptr"},
            {MESHCORE_DEVICE_ROLE_ROOM_SERVER, "Room"},
            {MESHCORE_DEVICE_ROLE_SENSOR, "Sens"},
        };
        int leg_row_h = TXT_TINY + 4;
        int leg_n     = (int)(sizeof(legend) / sizeof(legend[0]));
        int leg_pw    = 84;
        int leg_ph    = leg_n * leg_row_h + 8;
        int leg_px    = 8;
        int leg_py    = 8;  // canvas-local
        cv_rect(&olayer, leg_px, leg_py, leg_pw, leg_ph, 0xC0000000);
        for (int i = 0; i < leg_n; i++) {
            int row_cx = leg_px + 14;
            int row_cy = leg_py + 4 + leg_row_h / 2 + i * leg_row_h;
            cv_pin(&olayer, row_cx, row_cy, legend[i].r, false);
        }

        // Status strip pills (top-right). Walk the same right-to-left chain as
        // render_map.c so the text objects below land on each pill.
        char     z_buf[12], sat_buf[16], rx_buf[16], bat_buf[16];
        bool     have_bat = false, have_rx = false, stale = false;
        uint32_t col_bat = COL_GREEN, col_sat = COL_GREEN;
        int      sx_z = 0, sx_sat = 0, sx_rx = 0, sx_bat = 0, sx_stale = 0;
        int      sy_local = 6;
        int      sx       = map_w - 12;

        bsp_power_battery_information_t bat = {0};
        if (bsp_power_get_battery_information(&bat) == ESP_OK && bat.battery_available) {
            int pct = (int)bat.remaining_percentage;
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            snprintf(bat_buf, sizeof(bat_buf), "%d%%%s", pct, bat.battery_charging ? "+" : "");
            col_bat  = pct <= 20 ? COL_RED : (pct <= 50 ? COL_AMBER : COL_GREEN);
            sx      -= text_w(bat_buf, TXT_SMALL);
            cv_rect(&olayer, sx - 4, sy_local - 2, text_w(bat_buf, TXT_SMALL) + 8, TXT_SMALL + 4, 0x80000000);
            sx_bat    = sx;
            have_bat  = true;
            sx       -= 12;
        }

        if (lora_rx_ok) {
            int cnt = 0;
            if (xSemaphoreTake(rx_mutex, 0) == pdTRUE) {
                cnt = rx_count;
                xSemaphoreGive(rx_mutex);
            }
            snprintf(rx_buf, sizeof(rx_buf), "RX:%d", cnt);
            sx -= text_w(rx_buf, TXT_SMALL);
            cv_rect(&olayer, sx - 4, sy_local - 2, text_w(rx_buf, TXT_SMALL) + 8, TXT_SMALL + 4, 0x80000000);
            sx_rx    = sx;
            have_rx  = true;
            sx      -= 12;
        }

        uint8_t  sats = gps_live_sats;
        uint32_t now  = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        stale         = gps_live_last_fix_ms != 0 && (now - gps_live_last_fix_ms) > 60000u;
        col_sat       = sats >= 4 ? COL_GREEN : sats > 0 ? COL_AMBER : COL_RED;
        if (stale) col_sat = COL_RED;
        if (gps_live_bus_ok)
            snprintf(sat_buf, sizeof(sat_buf), "SAT:%u", (unsigned)sats);
        else
            snprintf(sat_buf, sizeof(sat_buf), "SAT:?");
        sx -= text_w(sat_buf, TXT_SMALL);
        cv_rect(&olayer, sx - 4, sy_local - 2, text_w(sat_buf, TXT_SMALL) + 8, TXT_SMALL + 4, 0x80000000);
        sx_sat  = sx;
        sx     -= 12;

        snprintf(z_buf, sizeof(z_buf), "Z:%u", (unsigned)map_zoom);
        sx -= text_w(z_buf, TXT_SMALL);
        cv_rect(&olayer, sx - 4, sy_local - 2, text_w(z_buf, TXT_SMALL) + 8, TXT_SMALL + 4, 0x80000000);
        sx_z = sx;

        if (stale) {
            sx_stale = sx - text_w("STALE", TXT_SMALL) - 14;
            cv_rect(&olayer, sx_stale - 4, sy_local - 2, text_w("STALE", TXT_SMALL) + 8, TXT_SMALL + 4, COL_RED);
        }

        // Nearest-node name panel (text object below).
        int  name_lx = 0, name_ly_local = 0;
        bool have_name = (best_name[0] && best_d2 < 80 * 80);
        if (have_name) {
            int pad       = 3;
            int nsz       = text_w(best_name, TXT_SMALL);
            name_lx       = best_x - nsz / 2;
            name_ly_local = best_y + 8;
            cv_rect(&olayer, name_lx - pad, name_ly_local - pad, nsz + 2 * pad, TXT_SMALL + 2 * pad, 0xC0000000);
        }

        lv_canvas_finish_layer(canvas, &olayer);

        // ── Text objects on top of the canvas (screen coords) ────────────────
        if (scale_buf[0]) {
            add_label(scr, scale_tx, map_y0 + scale_ty_local, TXT_SMALL, COL_WHITE, scale_buf);
        }
        for (int i = 0; i < leg_n; i++) {
            int row_cy = leg_py + 4 + leg_row_h / 2 + i * leg_row_h;
            add_label(scr, leg_px + 30, map_y0 + row_cy - TXT_TINY / 2 - 1, TXT_TINY, COL_WHITE, legend[i].l);
        }
        add_label(scr, sx_z, map_y0 + sy_local, TXT_SMALL, COL_WHITE, z_buf);
        add_label(scr, sx_sat, map_y0 + sy_local, TXT_SMALL, col_sat, sat_buf);
        if (have_rx) add_label(scr, sx_rx, map_y0 + sy_local, TXT_SMALL, COL_GREEN, rx_buf);
        if (have_bat) add_label(scr, sx_bat, map_y0 + sy_local, TXT_SMALL, col_bat, bat_buf);
        if (stale) add_label(scr, sx_stale, map_y0 + sy_local, TXT_SMALL, COL_WHITE, "STALE");
        if (have_name) add_label(scr, name_lx, map_y0 + name_ly_local, TXT_SMALL, COL_WHITE, best_name);
    } else {
        // Allocation failed — at least clear the map area to the panel colour.
        add_rect(scr, 0, map_y0, map_w, map_h, COL_PANEL);
    }

    // ── Header strip (over the canvas; full-width, outside the map area) ──────
    add_rect(scr, 0, 0, w, MAP_LV_HEADER_H, COL_PAGER_BG);
    add_rect(scr, 0, MAP_LV_HEADER_H - 1, w, 1, COL_PAGER_ACCENT);
    int hty = (MAP_LV_HEADER_H - TXT_TAB) / 2;
    add_label(scr, 12, hty, TXT_TAB, COL_PAGER_TEXT, "Map");
    if (map_lock_on) {
        int bw = text_w("LOCK", TXT_SMALL) + 12;
        int bx = 80;
        int by = (MAP_LV_HEADER_H - (TXT_SMALL + 4)) / 2;
        add_rect(scr, bx, by, bw, TXT_SMALL + 4, COL_AMBER);
        add_label(scr, bx + 6, by + 2, TXT_SMALL, COL_PAGER_BG, "LOCK");
    }

    // ── Footer / OSM attribution ─────────────────────────────────────────────
    int fy = h - MAP_LV_FOOTER_H;
    add_rect(scr, 0, fy, w, MAP_LV_FOOTER_H, COL_PAGER_BG);
    add_rect(scr, 0, fy, w, 1, COL_PAGER_ACCENT);
    add_label(scr, 10, fy + (MAP_LV_FOOTER_H - TXT_SMALL) / 2, TXT_SMALL, COL_WHITE, "(c) OpenStreetMap contributors");

    // First-fix toast — one-shot per process visit (mirrors render_map.c).
    if (!s_map_first_fix_seen && gps_live_valid) {
        s_map_first_fix_seen = true;
        snprintf(toast_text, sizeof(toast_text), "GPS fix locked");
        toast_start_ms    = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        toast_duration_ms = 2000;
    }
    map_toast_lvgl(scr, w, h);

    // Persist centre + zoom once the user pauses (debounced NVS write).
    map_state_tick();
}

// ── Dispatch ─────────────────────────────────────────────────────────────────

bool lvgl_view_active(app_view_t v) {
    switch (v) {
        case VIEW_ABOUT:
        case VIEW_HOME:
        case VIEW_MAP:
        case VIEW_TOOLBOX:
        case VIEW_TOOLBOX_COVERAGE:
            return true;
        case VIEW_SETTINGS:
            // Settings + its OwnTracks QR overlay both render through LVGL now.
            return true;
        case VIEW_NODES:
            // Nodes + its QR overlay both render through LVGL now.
            return true;
        case VIEW_CHAT:
            // Chat + the emoji-picker overlay both render through LVGL now.
            return true;
        case VIEW_CHANNEL:
            // Channel + its QR (share) overlay both render through LVGL now.
            return true;
        default:
            return false;
    }
}

void lvgl_view_render(app_view_t v) {
    switch (v) {
        case VIEW_SETTINGS:
            if (qr_overlay_active && qr_overlay_mode == QR_MODE_OWNTRACKS) {
                render_qr_overlay_lvgl();
            } else {
                render_settings_lvgl();
            }
            break;
        case VIEW_ABOUT:
            render_about_lvgl();
            break;
        case VIEW_HOME:
            render_home_lvgl();
            break;
        case VIEW_MAP:
            render_map_lvgl();
            break;
        case VIEW_TOOLBOX:
            render_toolbox_lvgl();
            break;
        case VIEW_TOOLBOX_COVERAGE:
            render_toolbox_coverage_lvgl();
            break;
        case VIEW_NODES:
            if (qr_overlay_active) {
                render_qr_overlay_lvgl();
            } else {
                render_nodes_lvgl();
            }
            break;
        case VIEW_CHAT:
            render_chat_lvgl();
            break;
        case VIEW_CHANNEL:
            if (qr_overlay_active) {
                render_qr_overlay_lvgl();
            } else {
                render_channel_lvgl();
            }
            break;
        default:
            return;
    }
    lvgl_port_refresh_now();
}
