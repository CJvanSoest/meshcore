// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// VIEW_HOME.

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

// ── VIEW_HOME ────────────────────────────────────────────────────────────────
// Tile-grid landing screen, pixel-matched to home_tiles.c. The tile metadata
// (labels, order, targets, unread badges) mirrors home_tiles[] there; keep the
// two in step. Icons are widget-built approximations of the PAX vector glyphs
// (LVGL's bitmap fonts do not scale to the ~60 px glyph sizes these need).

#define HOME_TILE_COLS  4
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
    IC_WIFI,
    IC_BLUETOOTH,
    IC_TOOLBOX,
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
        case IC_WIFI: {
            // Mirrors cat_icon_wifi_lv: a fan of concentric arcs over a dot,
            // opening upward. Arc origin sits low so the waves open up.
            int half = sz / 2, oy = cy + half / 3;
            add_arc(p, cx, oy, half * 9 / 10, 195, 345, 2, col);
            add_arc(p, cx, oy, half * 6 / 10, 195, 345, 2, col);
            add_arc(p, cx, oy, half * 3 / 10, 195, 345, 2, col);
            add_circle(p, cx, oy, sz / 12, col, -1, 0);
            break;
        }
        case IC_BLUETOOTH: {
            // Mirrors cat_icon_bluetooth_lv: the runic glyph (vertical spine +
            // two crossed bowties).
            int half = sz / 2, top = cy - half, bot = cy + half;
            int rx = half / 2, qy = cy - half / 2, by = cy + half / 2;
            add_line(p, cx, top, cx, bot, 2, col);
            add_line(p, cx, top, cx + rx, qy, 2, col);
            add_line(p, cx + rx, qy, cx - rx, by, 2, col);
            add_line(p, cx, bot, cx + rx, by, 2, col);
            add_line(p, cx + rx, by, cx - rx, qy, 2, col);
            break;
        }
        case IC_TOOLBOX: {
            // Mirrors cat_icon_toolbox_lv: a tool chest with a handle.
            int half = sz / 2;
            int bx0 = cx - half, bx1 = cx + half;
            int by0 = cy - half / 5, by1 = cy + half;
            add_line(p, bx0, by0, bx1, by0, 2, col);
            add_line(p, bx1, by0, bx1, by1, 2, col);
            add_line(p, bx1, by1, bx0, by1, 2, col);
            add_line(p, bx0, by1, bx0, by0, 2, col);
            add_line(p, bx0, cy + half / 4, bx1, cy + half / 4, 2, col);
            int hx0 = cx - half / 3, hx1 = cx + half / 3, hy = cy - half / 2;
            add_line(p, hx0, by0, hx0, hy, 2, col);
            add_line(p, hx1, by0, hx1, hy, 2, col);
            add_line(p, hx0, hy, hx1, hy, 2, col);
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
    {"Nodes", IC_NODES, 0},
    {"DM", IC_DM, 1},
    {"Channel", IC_CHANNEL, 2},
    {"Map", IC_MAP, 0},
    {"Advert", IC_ANTENNA, 0},
    {"WiFi", IC_WIFI, 0},
    {"Bluetooth", IC_BLUETOOTH, 0},
    {"Toolbox", IC_TOOLBOX, 0},
    {"Settings", IC_SETTINGS, 0},
    {"About", IC_ABOUT, 0},
    {"QR", IC_QR, 0},
    {"Exit", IC_EXIT, 0},
};

void home_status_right(lv_obj_t* scr, int x_right, int ty, int font) {
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

void render_home_lvgl(void) {
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

        // Label centred in the lower third. With 4 columns the tiles are
        // narrower (~180 px); the long labels ("Bluetooth", "Channel") overflow
        // TXT_BODY, so fall back to TXT_SMALL when the body width won't fit.
        const char* lbl      = home_meta[i].label;
        int         lbl_font = TXT_BODY;
        int         lw       = text_w(lbl, lbl_font);
        if (lw > tile_w - 8) {
            lbl_font = TXT_SMALL;
            lw       = text_w(lbl, lbl_font);
        }
        add_label(scr, tx + (tile_w - lw) / 2, tyt + tile_h * 2 / 3, lbl_font, fg, lbl);

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
