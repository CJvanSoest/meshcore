// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// VIEW_HOME — tile-grid landing screen in LilyGo Pager visual style.
//
// Layout (720x720):
//   top status strip (50 px)  — owner | RX:n | TX:dc%  | bat%
//   tile grid    (4 cols × 2 rows, ~290 px tall tiles, launcher proportions)
//   footer hint  (38 px)      — keyboard hint
//
// Each tile draws a small PAX-shape "icon" in the upper half and a label
// in the lower half (no label outside the tile — concept overnomen van de
// Pager, label-onder-tile is daar dubbel met de in-tile titel).

#include "render.h"
#include "render_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "bsp/power.h"
#include "esp_err.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"

#include "app_config.h"
#include "channels.h"
#include "contacts.h"
#include "radio.h"
#include "settings_nvs.h"
#include "ui_state.h"

// ── Tile-grid geometry (mirrors launcher theme: 4 cols, 30/20 margins) ───────
#define HOME_TILE_COLS     4
#define HOME_TILE_ROWS     2
#define HOME_TILE_COUNT    (HOME_TILE_COLS * HOME_TILE_ROWS)
#define HOME_H_MARGIN      30
#define HOME_V_MARGIN      20
#define HOME_HEADER_H      50
#define HOME_FOOTER_H      38

// ── Icon drawing helpers — simple PAX-shape glyphs, ~60 px diameter ──────────
static void icon_nodes(int cx, int cy, int sz, pax_col_t col) {
    int r = sz / 10;
    int off = sz / 3;
    pax_simple_line(&fb, col, cx - off, cy - off / 2, cx + off, cy - off / 2);
    pax_simple_line(&fb, col, cx - off, cy - off / 2, cx,       cy + off);
    pax_simple_line(&fb, col, cx + off, cy - off / 2, cx,       cy + off);
    pax_simple_circle(&fb, col, cx - off, cy - off / 2, r);
    pax_simple_circle(&fb, col, cx + off, cy - off / 2, r);
    pax_simple_circle(&fb, col, cx,       cy + off,     r);
}

static void icon_dm(int cx, int cy, int sz, pax_col_t col) {
    int w = sz, h = sz * 7 / 10;
    int x = cx - w / 2, y = cy - h / 2;
    pax_simple_rect(&fb, col, x, y, w, h);
    pax_simple_tri (&fb, col, x + w / 5, y + h, x + w / 3, y + h, x + w / 4, y + h + h / 3);
}

static void icon_channel(int cx, int cy, int sz, pax_col_t col) {
    pax_vec2f csz = pax_text_size(FONT, sz, "#");
    pax_draw_text(&fb, col, FONT, sz, cx - (int)csz.x / 2, cy - sz / 2, "#");
}

static void icon_map(int cx, int cy, int sz, pax_col_t col) {
    pax_outline_circle(&fb, col, cx, cy, sz / 2);
    pax_simple_line   (&fb, col, cx - sz / 2, cy, cx + sz / 2, cy);
    pax_outline_circle(&fb, col, cx, cy, sz / 4);
}

static void icon_advert(int cx, int cy, int sz, pax_col_t col) {
    int b = sz / 8;
    int g = sz / 4;
    for (int row = 0; row < 3; row++) {
        for (int c = 0; c < 3; c++) {
            if ((row + c) % 2 == 0) {
                pax_simple_rect(&fb, col,
                                cx - sz / 2 + c * g + b, cy - sz / 2 + row * g + b, b, b);
            }
        }
    }
}

static void icon_settings(int cx, int cy, int sz, pax_col_t col) {
    pax_outline_hollow_circle(&fb, col, cx, cy, sz / 3, sz / 4);
    for (int a = 0; a < 8; a++) {
        float t = (float)a * 3.14159f / 4.0f;
        float ox = (float)cx + (sz / 2.2f) * cosf(t);
        float oy = (float)cy + (sz / 2.2f) * sinf(t);
        pax_simple_circle(&fb, col, ox, oy, sz / 12);
    }
}

static void icon_about(int cx, int cy, int sz, pax_col_t col) {
    pax_outline_circle(&fb, col, cx, cy, sz / 2);
    pax_vec2f csz = pax_text_size(FONT, sz * 3 / 4, "i");
    pax_draw_text(&fb, col, FONT, sz * 3 / 4, cx - (int)csz.x / 2, cy - sz * 3 / 8, "i");
}

static void icon_placeholder(int cx, int cy, int sz, pax_col_t col) {
    // Hollow rounded-rect-ish — four corner lines, signalling "coming soon".
    int h = sz / 2;
    int q = sz / 4;
    pax_simple_line(&fb, col, cx - h, cy - h, cx - q, cy - h);
    pax_simple_line(&fb, col, cx + q, cy - h, cx + h, cy - h);
    pax_simple_line(&fb, col, cx - h, cy + h, cx - q, cy + h);
    pax_simple_line(&fb, col, cx + q, cy + h, cx + h, cy + h);
    pax_simple_line(&fb, col, cx - h, cy - h, cx - h, cy - q);
    pax_simple_line(&fb, col, cx - h, cy + q, cx - h, cy + h);
    pax_simple_line(&fb, col, cx + h, cy - h, cx + h, cy - q);
    pax_simple_line(&fb, col, cx + h, cy + q, cx + h, cy + h);
}

// ── Tile definitions ─────────────────────────────────────────────────────────
typedef void (*home_icon_fn)(int cx, int cy, int sz, pax_col_t col);

typedef int (*home_badge_fn)(void);

typedef struct {
    const char    *label;
    app_view_t     target;        // view to open on Enter; VIEW_HOME = TBD
    home_icon_fn   draw_icon;
    home_action_t  action;        // post-open side-effect (e.g. open QR overlay)
    home_badge_fn  unread;        // optional: returns unread count for badge
} home_tile_t;

static const home_tile_t home_tiles[HOME_TILE_COUNT] = {
    { "Nodes",    VIEW_NODES,    icon_nodes,    HOME_ACTION_NONE,        NULL                  },
    { "DM",       VIEW_CHAT,     icon_dm,       HOME_ACTION_NONE,        contact_unread_total  },
    { "Channel",  VIEW_CHANNEL,  icon_channel,  HOME_ACTION_NONE,        channel_unread_total  },
    { "Map",      VIEW_HOME,     icon_map,      HOME_ACTION_NONE,        NULL                  },  // TODO: VIEW_MAP
    { "Advert",   VIEW_HOME,     icon_advert,   HOME_ACTION_SEND_ADVERT, NULL                  },
    { "Settings", VIEW_SETTINGS, icon_settings, HOME_ACTION_NONE,        NULL                  },
    { "About",    VIEW_ABOUT,    icon_about,    HOME_ACTION_NONE,        NULL                  },
    { "QR",       VIEW_NODES,    icon_advert,   HOME_ACTION_OPEN_QR,     NULL                  },
};

// Expose the tile count + target/action lookup to input.c so Enter opens the
// right view and triggers any post-open side-effect.
int home_tile_count(void) { return HOME_TILE_COUNT; }

app_view_t home_tile_target(int idx) {
    if (idx < 0 || idx >= HOME_TILE_COUNT) return VIEW_HOME;
    return home_tiles[idx].target;
}

home_action_t home_tile_action(int idx) {
    if (idx < 0 || idx >= HOME_TILE_COUNT) return HOME_ACTION_NONE;
    return home_tiles[idx].action;
}

static void render_home_header(int w) {
    // Pager-style status strip: owner on the left; RX, TX (duty-cycle %), and
    // battery % on the right. Background is the Pager BG with a 1 px accent
    // separator at the bottom.
    pax_simple_rect(&fb, COL_PAGER_BG,     0, 0, w, HOME_HEADER_H);
    pax_simple_rect(&fb, COL_PAGER_ACCENT, 0, HOME_HEADER_H - 1, w, 1);

    int tx = 12;
    int ty = (HOME_HEADER_H - TXT_BODY) / 2;
    // Mirror send_advert / render_qr_overlay: advert_name overrides owner_name.
    // Keeps the home strip identity = what the LoRa network sees.
    const char *name = lora_advert_name[0] ? lora_advert_name
                     : (owner_name[0]      ? owner_name      : "(no name)");
    pax_draw_text(&fb, COL_PAGER_TEXT, FONT, TXT_BODY, tx, ty, name);

    int x = w - 12;

    bsp_power_battery_information_t bat = {0};
    if (bsp_power_get_battery_information(&bat) == ESP_OK && bat.battery_available) {
        int pct = (int)bat.remaining_percentage;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        char buf[16];
        snprintf(buf, sizeof(buf), "%d%%%s", pct, bat.battery_charging ? "+" : "");
        pax_col_t col = pct <= 20 ? COL_RED : (pct <= 50 ? COL_AMBER : COL_GREEN);
        pax_vec2f sz = pax_text_size(FONT, TXT_BODY, buf);
        x -= (int)sz.x;
        pax_draw_text(&fb, col, FONT, TXT_BODY, x, ty, buf);
        x -= 14;
    }

    // TX: rolling duty-cycle %  ("TX-time") — mirrors Pager's airtime display.
    if (dc_budget_ms > 0 && dc_budget_ms < 3600000u) {
        unsigned pct_x10 = (unsigned)(((uint64_t)dc_used_ms * 1000u) / dc_budget_ms);
        char buf[16];
        snprintf(buf, sizeof(buf), "TX:%u.%u%%", pct_x10 / 10u, pct_x10 % 10u);
        pax_col_t col = dc_last_tx_blocked ? COL_RED :
                        (pct_x10 >= 800)   ? COL_AMBER : COL_PAGER_TEXT;
        pax_vec2f sz = pax_text_size(FONT, TXT_BODY, buf);
        x -= (int)sz.x;
        pax_draw_text(&fb, col, FONT, TXT_BODY, x, ty, buf);
        x -= 14;
    }

    if (lora_rx_ok) {
        int cnt = 0;
        if (xSemaphoreTake(rx_mutex, 0) == pdTRUE) {
            cnt = rx_count;
            xSemaphoreGive(rx_mutex);
        }
        char buf[16];
        snprintf(buf, sizeof(buf), "RX:%d", cnt);
        pax_vec2f sz = pax_text_size(FONT, TXT_BODY, buf);
        x -= (int)sz.x;
        pax_draw_text(&fb, COL_GREEN, FONT, TXT_BODY, x, ty, buf);
    }
}

void render_home(void) {
    int w = (int)pax_buf_get_width(&fb);
    int h = (int)pax_buf_get_height(&fb);

    pax_background(&fb, COL_PAGER_BG);
    render_home_header(w);

    // Tile-area geometry — launcher proportions: 4 cols × 2 rows,
    // 30 px horizontal margin, 20 px vertical margin.
    int area_y0 = HOME_HEADER_H + HOME_V_MARGIN;
    int area_h  = h - area_y0 - HOME_V_MARGIN - HOME_FOOTER_H;
    int area_w  = w - HOME_H_MARGIN * 2;

    int tile_w = (area_w - HOME_H_MARGIN * (HOME_TILE_COLS - 1)) / HOME_TILE_COLS;
    int tile_h = (area_h - HOME_V_MARGIN * (HOME_TILE_ROWS - 1)) / HOME_TILE_ROWS;

    if (home_cursor < 0)                  home_cursor = 0;
    if (home_cursor >= HOME_TILE_COUNT)   home_cursor = HOME_TILE_COUNT - 1;

    for (int i = 0; i < HOME_TILE_COUNT; i++) {
        int col = i % HOME_TILE_COLS;
        int row = i / HOME_TILE_COLS;
        int tx  = HOME_H_MARGIN + col * (tile_w + HOME_H_MARGIN);
        int ty  = area_y0       + row * (tile_h + HOME_V_MARGIN);

        bool focused = (i == home_cursor);
        pax_col_t bg     = focused ? COL_PAGER_ACCENT : COL_PAGER_TILE;
        pax_col_t fg     = focused ? COL_HEADER       : COL_PAGER_TEXT;

        pax_simple_rect(&fb, bg, tx, ty, tile_w, tile_h);
        if (focused) {
            // Slight inset highlight border so the focused tile reads even on
            // a small screen mock-up.
            pax_simple_rect(&fb, COL_PAGER_BG, tx + 2, ty + 2, tile_w - 4, 2);
            pax_simple_rect(&fb, COL_PAGER_BG, tx + 2, ty + tile_h - 4, tile_w - 4, 2);
            pax_simple_rect(&fb, COL_PAGER_BG, tx + 2, ty + 2, 2, tile_h - 4);
            pax_simple_rect(&fb, COL_PAGER_BG, tx + tile_w - 4, ty + 2, 2, tile_h - 4);
        }

        // Disabled (TBD) placeholders: dimmer foreground.
        bool tbd = (home_tiles[i].target == VIEW_HOME);
        if (tbd && !focused) fg = COL_GRAY;

        // Icon: centered horizontally, vertically biased to upper half of tile.
        int icon_sz = tile_w / 2;
        if (icon_sz > tile_h * 2 / 5) icon_sz = tile_h * 2 / 5;
        int icon_cx = tx + tile_w / 2;
        int icon_cy = ty + tile_h * 2 / 5;
        if (home_tiles[i].draw_icon) {
            home_tiles[i].draw_icon(icon_cx, icon_cy, icon_sz, fg);
        }

        // Label: centered horizontally, in the lower third of the tile.
        if (home_tiles[i].label[0]) {
            pax_vec2f lsz = pax_text_size(FONT, TXT_BODY, home_tiles[i].label);
            int lx = tx + (tile_w - (int)lsz.x) / 2;
            int ly = ty + tile_h * 3 / 4;
            pax_draw_text(&fb, fg, FONT, TXT_BODY, lx, ly, home_tiles[i].label);
            if (tbd) {
                const char *sub = "soon";
                pax_vec2f ssz = pax_text_size(FONT, TXT_TINY, sub);
                pax_draw_text(&fb, COL_GRAY, FONT, TXT_TINY,
                              tx + (tile_w - (int)ssz.x) / 2,
                              ly + TXT_BODY + 2, sub);
            }
        }

        // Unread badge in the top-right corner of the tile (DM, Channel).
        // Mirrors the tab-bar pill so the count is glanceable from home.
        if (home_tiles[i].unread) {
            int count = home_tiles[i].unread();
            if (count > 0) {
                char buf[8];
                snprintf(buf, sizeof(buf), "%d", count > 99 ? 99 : count);
                pax_vec2f bsz = pax_text_size(FONT, TXT_SMALL, buf);
                int bw = (int)bsz.x + 14;
                int bh = TXT_SMALL + 8;
                int bx = tx + tile_w - bw - 10;
                int by = ty + 10;
                pax_simple_rect(&fb, COL_RED, bx, by, bw, bh);
                pax_draw_text(&fb, COL_PAGER_BG, FONT, TXT_SMALL,
                              bx + (bw - (int)bsz.x) / 2, by + 4, buf);
            }
        }
    }

    // Footer: keyboard hint on the left, RX / SNR / noise on the right.
    int fy = h - HOME_FOOTER_H;
    pax_simple_rect(&fb, COL_HEADER,       0, fy, w, HOME_FOOTER_H);
    pax_simple_rect(&fb, COL_PAGER_ACCENT, 0, fy, w, 1);
    int hint_y = fy + (HOME_FOOTER_H - TXT_SMALL) / 2;
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 10, hint_y,
                  "WSAD: nav   Enter: open   Tab: classic tabs");

    // Mirror the bottom-row stats from the Settings tab so the home screen
    // doubles as a quick-glance radio dashboard.
    char rf[64] = {0};
    int  snr_dB = (int)last_rx_snr_db_x4 / 4;
    if (last_rx_stats_valid && noise_floor_valid) {
        snprintf(rf, sizeof(rf), "RX:%d SNR:%+d N:%d",
                 (int)last_rx_rssi_dbm, snr_dB, (int)noise_floor_dbm);
    } else if (last_rx_stats_valid) {
        snprintf(rf, sizeof(rf), "RX:%d SNR:%+d", (int)last_rx_rssi_dbm, snr_dB);
    } else if (noise_floor_valid) {
        snprintf(rf, sizeof(rf), "noise:%d", (int)noise_floor_dbm);
    }
    if (rf[0]) {
        pax_vec2f rsz = pax_text_size(FONT, TXT_SMALL, rf);
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL,
                      w - (int)rsz.x - 10, hint_y, rf);
    }

    // 2-second status toast (e.g. "Flood advert sent"). Centered, Pager-style
    // panel with an accent stripe so it reads as a confirmation, not an error.
    if (toast_text[0]) {
        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (now_ms - toast_start_ms < 2000) {
            pax_vec2f tsz = pax_text_size(FONT, TXT_TITLE, toast_text);
            int box_w = (int)tsz.x + 60;
            int box_h = TXT_TITLE + 40;
            int box_x = (w - box_w) / 2;
            int box_y = (h - box_h) / 2;
            pax_simple_rect(&fb, COL_PAGER_BG,     box_x, box_y, box_w, box_h);
            pax_simple_rect(&fb, COL_PAGER_ACCENT, box_x, box_y, box_w, 3);
            pax_simple_rect(&fb, COL_PAGER_ACCENT, box_x, box_y + box_h - 3, box_w, 3);
            pax_draw_text(&fb, COL_PAGER_ACCENT, FONT, TXT_TITLE,
                          box_x + 30, box_y + 20, toast_text);
        } else {
            toast_text[0] = 0;
        }
    }
}
