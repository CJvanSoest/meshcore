// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// VIEW_MAP — OSM-tile based map view with live GPS overlay. The tile pipeline
// (slippy-map math + PNG decode + LRU cache) lives in map.c; this file is the
// per-frame paint of the view + the on-screen overlays (status strip,
// crosshair, scale bar, node pins). The red X returns to home.

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "bsp/power.h"
#include "contacts.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gps_task.h"
#include "map.h"
#include "nodes.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"
#include "radio.h"
#include "render.h"
#include "render_internal.h"
#include "ui_state.h"

#define MAP_HEADER_H    44
#define MAP_FOOTER_H    26  // OSM attribution strip (TXT_SMALL height + padding)
#define MAP_ATTRIBUTION "(c) OpenStreetMap contributors"

// First-fix toast latch — set once per VIEW_MAP entry so we don't toast on
// every iframe. Reset to false the moment the user leaves the view.
static bool s_first_fix_seen = false;

static void render_map_header(int w) {
    pax_simple_rect(&fb, COL_PAGER_BG, 0, 0, w, MAP_HEADER_H);
    pax_simple_rect(&fb, COL_PAGER_ACCENT, 0, MAP_HEADER_H - 1, w, 1);
    int ty = (MAP_HEADER_H - TXT_TAB) / 2;
    pax_draw_text(&fb, COL_PAGER_TEXT, FONT, TXT_TAB, 12, ty, "Map");
    if (map_lock_on) {
        // Small amber pill in the header so the user can tell at a glance
        // whether the centre is following the live GPS fix or pinned.
        const char* pill = "LOCK";
        pax_vec2f   sz   = pax_text_size(FONT, TXT_SMALL, pill);
        int         bw   = (int)sz.x + 12;
        int         bx   = 80;
        int         by   = (MAP_HEADER_H - (TXT_SMALL + 4)) / 2;
        pax_simple_rect(&fb, COL_AMBER, bx, by, bw, TXT_SMALL + 4);
        pax_draw_text(&fb, COL_PAGER_BG, FONT, TXT_SMALL, bx + 6, by + 2, pill);
    }
}

static void render_map_attribution(int w, int h) {
    int fy = h - MAP_FOOTER_H;
    pax_simple_rect(&fb, COL_PAGER_BG, 0, fy, w, MAP_FOOTER_H);
    pax_simple_rect(&fb, COL_PAGER_ACCENT, 0, fy, w, 1);
    int ty = fy + (MAP_FOOTER_H - TXT_SMALL) / 2;
    pax_draw_text(&fb, COL_WHITE, FONT, TXT_SMALL, 10, ty, MAP_ATTRIBUTION);
}

// Convert a (lat, lon) into framebuffer pixel coords inside the map area,
// using the same zoom + centre as the tile raster paint below. Returns
// false if the projected pixel is outside the visible window.
static bool latlon_to_fb(double lat, double lon, double ctr_lat, double ctr_lon, int zoom, int map_x0, int map_y0,
                         int map_w, int map_h, int* out_x, int* out_y) {
    int ctx, cty, cpx, cpy;
    map_latlon_to_tile(ctr_lat, ctr_lon, zoom, &ctx, &cty, &cpx, &cpy);
    int tx, ty, px, py;
    map_latlon_to_tile(lat, lon, zoom, &tx, &ty, &px, &py);

    int dx_px = (tx - ctx) * MAP_TILE_PX + (px - cpx);
    int dy_px = (ty - cty) * MAP_TILE_PX + (py - cpy);
    int fx    = map_x0 + map_w / 2 + dx_px;
    int fy    = map_y0 + map_h / 2 + dy_px;
    if (fx < map_x0 || fx >= map_x0 + map_w) return false;
    if (fy < map_y0 || fy >= map_y0 + map_h) return false;
    if (out_x) *out_x = fx;
    if (out_y) *out_y = fy;
    return true;
}

// Paint the 3x3 (or 4x4 worst-case) tile raster around the given centre
// pixel inside the map area `[map_x0, map_x0 + map_w]` × `[map_y0, ... ]`.
// Missing tiles render as a grey rect with a one-pixel grid line so SD-load
// errors don't leave the framebuffer un-cleared.
static void render_tile_raster(double lat, double lon, int zoom, int map_x0, int map_y0, int map_w, int map_h) {
    int center_tx, center_ty, px_in, py_in;
    map_latlon_to_tile(lat, lon, zoom, &center_tx, &center_ty, &px_in, &py_in);

    int origin_x = map_x0 + map_w / 2 - px_in;
    int origin_y = map_y0 + map_h / 2 - py_in;

    // Sweep a 5×5 (-2..+2) window around the centre tile. Tiles outside the
    // visible map area are still requested (map_tile_get warms them into
    // the LRU cache) so a one-step pan never needs a fresh SD-read +
    // lodepng decode for the new edge tiles — they're already resident.
    int dx_min = -2, dx_max = 2;
    int dy_min = -2, dy_max = 2;

    // Hold the cache mutex for the whole sweep — the background loader
    // task can't evict an in-use slot mid-pax_draw_image, and cache misses
    // for both visible *and* off-screen ring tiles queue async loads.
    map_cache_lock();
    pax_clip(&fb, map_x0, map_y0, map_w, map_h);
    for (int dy = dy_min; dy <= dy_max; dy++) {
        for (int dx = dx_min; dx <= dx_max; dx++) {
            int        tile_x  = center_tx + dx;
            int        tile_y  = center_ty + dy;
            int        tx      = origin_x + dx * MAP_TILE_PX;
            int        ty      = origin_y + dy * MAP_TILE_PX;
            bool       visible = !(tx + MAP_TILE_PX < map_x0 || tx >= map_x0 + map_w || ty + MAP_TILE_PX < map_y0 ||
                             ty >= map_y0 + map_h);
            pax_buf_t* t       = map_tile_get(zoom, tile_x, tile_y);
            if (!visible) continue;  // off-screen tiles only warm the cache
            if (t) {
                pax_draw_image(&fb, t, (float)tx, (float)ty);
            } else {
                // Missing tile: flat dark fill, no grid lines. At low zoom
                // levels where most NL data isn't rendered the grid pattern
                // dominated the screen — flat looks intentional.
                pax_simple_rect(&fb, COL_PANEL, tx, ty, MAP_TILE_PX, MAP_TILE_PX);
            }
        }
    }
    pax_noclip(&fb);
    map_cache_unlock();
}

// Cross-hatch in the map centre so the user knows what point the
// coordinates refer to. Two-tone (black underlay + white fill) so it's
// readable over both dark and light tiles.
static void render_crosshair(int map_x0, int map_y0, int map_w, int map_h) {
    int cx  = map_x0 + map_w / 2;
    int cy  = map_y0 + map_h / 2;
    int arm = 8;
    // 1 px black halo (drawn as 3 px line with 1 px stroke of bg colour)
    pax_simple_rect(&fb, 0xFF000000, cx - arm - 1, cy - 1, 2 * arm + 2, 3);
    pax_simple_rect(&fb, 0xFF000000, cx - 1, cy - arm - 1, 3, 2 * arm + 2);
    // White core
    pax_simple_rect(&fb, COL_WHITE, cx - arm, cy, 2 * arm + 1, 1);
    pax_simple_rect(&fb, COL_WHITE, cx, cy - arm, 1, 2 * arm + 1);
}

// Scale bar in the bottom-left of the map area. Metres-per-pixel at this
// zoom + latitude → choose 100 m / 250 m / 500 m / 1 km / 2 km / 5 km step
// closest to ~120 px on screen.
static void render_scale_bar(double lat, int zoom, int map_x0, int map_y0, int map_w, int map_h) {
    (void)map_x0;
    (void)map_w;
    double lat_rad  = lat * M_PI / 180.0;
    double earth_eq = 40075016.686;  // metres
    double m_per_px = (earth_eq * cos(lat_rad)) / ((double)MAP_TILE_PX * (double)(1 << zoom));
    if (m_per_px <= 0) return;

    static const int steps[]  = {100, 250, 500, 1000, 2000, 5000, 10000, 25000};
    int              chosen   = steps[0];
    int              chosen_w = (int)(chosen / m_per_px);
    for (size_t i = 1; i < sizeof(steps) / sizeof(steps[0]); i++) {
        int w_px = (int)(steps[i] / m_per_px);
        if (abs(w_px - 120) < abs(chosen_w - 120)) {
            chosen   = steps[i];
            chosen_w = w_px;
        }
    }
    int bar_h = 6;
    int bx    = 12;
    int by    = map_y0 + map_h - 28;
    pax_simple_rect(&fb, 0xFF000000, bx - 1, by - 1, chosen_w + 2, bar_h + 2);
    pax_simple_rect(&fb, COL_WHITE, bx, by, chosen_w, bar_h);
    char buf[16];
    if (chosen >= 1000)
        snprintf(buf, sizeof(buf), "%d km", chosen / 1000);
    else
        snprintf(buf, sizeof(buf), "%d m", chosen);
    // Dark pill behind the label so the white text stays legible on top of
    // bright OSM tiles. TXT_SMALL is the same size used by the status strip
    // values so the visual weight is consistent.
    pax_vec2f tsz = pax_text_size(FONT, TXT_SMALL, buf);
    int       tx  = bx;
    int       ty  = by - TXT_SMALL - 4;
    pax_simple_rect(&fb, 0xC0000000, tx - 4, ty - 2, (int)tsz.x + 8, TXT_SMALL + 4);
    pax_draw_text(&fb, COL_WHITE, FONT, TXT_SMALL, tx, ty, buf);
}

// Right-side status strip: SAT count (colour-graded) + RX count + battery %.
// Drawn over the map area so it doesn't steal vertical space from tiles.
static void render_status_strip(int w, int map_y0) {
    int x = w - 12;
    int y = map_y0 + 6;

    // Battery first (rightmost) so we anchor the chain to the screen edge.
    bsp_power_battery_information_t bat = {0};
    if (bsp_power_get_battery_information(&bat) == ESP_OK && bat.battery_available) {
        int pct = (int)bat.remaining_percentage;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        char buf[16];
        snprintf(buf, sizeof(buf), "%d%%%s", pct, bat.battery_charging ? "+" : "");
        pax_col_t col  = pct <= 20 ? COL_RED : (pct <= 50 ? COL_AMBER : COL_GREEN);
        pax_vec2f sz   = pax_text_size(FONT, TXT_SMALL, buf);
        x             -= (int)sz.x;
        pax_simple_rect(&fb, 0x80000000, x - 4, y - 2, (int)sz.x + 8, TXT_SMALL + 4);
        pax_draw_text(&fb, col, FONT, TXT_SMALL, x, y, buf);
        x -= 12;
    }

    if (lora_rx_ok) {
        int cnt = 0;
        if (xSemaphoreTake(rx_mutex, 0) == pdTRUE) {
            cnt = rx_count;
            xSemaphoreGive(rx_mutex);
        }
        char buf[16];
        snprintf(buf, sizeof(buf), "RX:%d", cnt);
        pax_vec2f sz  = pax_text_size(FONT, TXT_SMALL, buf);
        x            -= (int)sz.x;
        pax_simple_rect(&fb, 0x80000000, x - 4, y - 2, (int)sz.x + 8, TXT_SMALL + 4);
        pax_draw_text(&fb, COL_GREEN, FONT, TXT_SMALL, x, y, buf);
        x -= 12;
    }

    // SAT count: red if no sats / stale fix; amber 1-3; green ≥4.
    uint8_t   sats    = gps_live_sats;
    uint32_t  now     = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    bool      stale   = gps_live_last_fix_ms != 0 && (now - gps_live_last_fix_ms) > 60000u;
    pax_col_t sat_col = sats >= 4 ? COL_GREEN : sats > 0 ? COL_AMBER : COL_RED;
    if (stale) sat_col = COL_RED;
    char buf[16];
    if (gps_live_bus_ok)
        snprintf(buf, sizeof(buf), "SAT:%u", (unsigned)sats);
    else
        snprintf(buf, sizeof(buf), "SAT:?");
    pax_vec2f sz  = pax_text_size(FONT, TXT_SMALL, buf);
    x            -= (int)sz.x;
    pax_simple_rect(&fb, 0x80000000, x - 4, y - 2, (int)sz.x + 8, TXT_SMALL + 4);
    pax_draw_text(&fb, sat_col, FONT, TXT_SMALL, x, y, buf);
    x -= 12;

    // Zoom level — sits on the same line as SAT/RX/bat so the user can tell
    // at a glance how far in/out they're looking without opening Settings.
    char zbuf[12];
    snprintf(zbuf, sizeof(zbuf), "Z:%u", (unsigned)map_zoom);
    pax_vec2f zsz  = pax_text_size(FONT, TXT_SMALL, zbuf);
    x             -= (int)zsz.x;
    pax_simple_rect(&fb, 0x80000000, x - 4, y - 2, (int)zsz.x + 8, TXT_SMALL + 4);
    pax_draw_text(&fb, COL_WHITE, FONT, TXT_SMALL, x, y, zbuf);

    if (stale) {
        const char* st  = "STALE";
        pax_vec2f   ssz = pax_text_size(FONT, TXT_SMALL, st);
        int         sx  = x - (int)ssz.x - 14;
        pax_simple_rect(&fb, COL_RED, sx - 4, y - 2, (int)ssz.x + 8, TXT_SMALL + 4);
        pax_draw_text(&fb, COL_WHITE, FONT, TXT_SMALL, sx, y, st);
    }
}

// Colour palette for the four MeshCore roles. Repeated in the legend +
// the per-node pin so the user can map shape↔role at a glance.
#define ROLE_COL_CHAT   COL_GREEN
#define ROLE_COL_RPTR   COL_BLUE
#define ROLE_COL_ROOM   0xFFBB9AF7  // soft purple (matches render_nodes)
#define ROLE_COL_SENSOR COL_AMBER

static pax_col_t role_pin_color(meshcore_device_role_t r) {
    switch (r) {
        case MESHCORE_DEVICE_ROLE_REPEATER:
            return ROLE_COL_RPTR;
        case MESHCORE_DEVICE_ROLE_ROOM_SERVER:
            return ROLE_COL_ROOM;
        case MESHCORE_DEVICE_ROLE_SENSOR:
            return ROLE_COL_SENSOR;
        case MESHCORE_DEVICE_ROLE_CHAT_NODE:
        default:
            return ROLE_COL_CHAT;
    }
}

// Draw a 7×7 px filled shape for the given role, centred on (cx, cy), with
// a 1 px black halo so it pops on both light and dark tile backgrounds.
// Shape mapping:
//   Chat   → filled circle
//   Rptr   → filled square
//   Room   → filled diamond (45° rotated square)
//   Sensor → filled upward triangle
static void draw_role_pin(int cx, int cy, meshcore_device_role_t role, bool favorite) {
    pax_col_t col = role_pin_color(role);
    // Black halo: draw a slightly larger shape underneath.
    switch (role) {
        case MESHCORE_DEVICE_ROLE_REPEATER:
            pax_simple_rect(&fb, 0xFF000000, cx - 4, cy - 4, 9, 9);
            pax_simple_rect(&fb, col, cx - 3, cy - 3, 7, 7);
            break;
        case MESHCORE_DEVICE_ROLE_ROOM_SERVER:
            pax_simple_tri(&fb, 0xFF000000, cx - 5, cy, cx, cy - 5, cx + 5, cy);
            pax_simple_tri(&fb, 0xFF000000, cx - 5, cy, cx, cy + 5, cx + 5, cy);
            pax_simple_tri(&fb, col, cx - 4, cy, cx, cy - 4, cx + 4, cy);
            pax_simple_tri(&fb, col, cx - 4, cy, cx, cy + 4, cx + 4, cy);
            break;
        case MESHCORE_DEVICE_ROLE_SENSOR:
            pax_simple_tri(&fb, 0xFF000000, cx - 5, cy + 4, cx + 5, cy + 4, cx, cy - 5);
            pax_simple_tri(&fb, col, cx - 4, cy + 3, cx + 4, cy + 3, cx, cy - 4);
            break;
        case MESHCORE_DEVICE_ROLE_CHAT_NODE:
        default:
            pax_simple_circle(&fb, 0xFF000000, cx, cy, 5);
            pax_simple_circle(&fb, col, cx, cy, 4);
            break;
    }
    if (favorite) {
        // White ring around the pin marks favorited contacts so they're
        // distinguishable from background-discovered nodes of the same role.
        pax_outline_circle(&fb, COL_WHITE, cx, cy, 7);
    }
}

// Compact legend in the top-left of the map area: 4 rows of shape+label,
// black semi-transparent panel so it stays readable on bright tiles. The
// status strip lives on the top-right, so left avoids any overlap.
static void render_legend(int w, int map_y0) {
    (void)w;
    static const struct {
        meshcore_device_role_t r;
        const char*            l;
    } items[] = {
        {MESHCORE_DEVICE_ROLE_CHAT_NODE, "Chat"},
        {MESHCORE_DEVICE_ROLE_REPEATER, "Rptr"},
        {MESHCORE_DEVICE_ROLE_ROOM_SERVER, "Room"},
        {MESHCORE_DEVICE_ROLE_SENSOR, "Sens"},
    };
    int row_h   = TXT_TINY + 4;
    int n_rows  = (int)(sizeof(items) / sizeof(items[0]));
    int panel_w = 84;
    int panel_h = n_rows * row_h + 8;
    int panel_x = 8;
    int panel_y = map_y0 + 8;
    pax_simple_rect(&fb, 0xC0000000, panel_x, panel_y, panel_w, panel_h);
    for (int i = 0; i < n_rows; i++) {
        int row_cx = panel_x + 14;
        int row_cy = panel_y + 4 + row_h / 2 + i * row_h;
        draw_role_pin(row_cx, row_cy, items[i].r, false);
        pax_draw_text(&fb, COL_WHITE, FONT, TXT_TINY, panel_x + 30, row_cy - TXT_TINY / 2 - 1, items[i].l);
    }
}

// Plot a shaped pin per node with position_valid. The node closest to the
// map crosshair gets its alias drawn as a label so the user can pan to
// identify any node without cluttering the map with every name at once.
static void render_node_pins(double ctr_lat, double ctr_lon, int zoom, int map_x0, int map_y0, int map_w, int map_h) {
    if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(5)) != pdTRUE) return;

    int  chx                              = map_x0 + map_w / 2;
    int  chy                              = map_y0 + map_h / 2;
    int  best_d2                          = INT32_MAX;
    int  best_fx                          = 0;
    int  best_fy                          = 0;
    char best_name[CONTACT_ALIAS_LEN + 1] = {0};

    pax_clip(&fb, map_x0, map_y0, map_w, map_h);
    for (int i = 0; i < node_count; i++) {
        node_entry_t* n = &node_list[i];
        if (!n->position_valid) continue;
        double lat = (double)n->lat / 1e6;
        double lon = (double)n->lon / 1e6;
        int    fx, fy;
        if (!latlon_to_fb(lat, lon, ctr_lat, ctr_lon, zoom, map_x0, map_y0, map_w, map_h, &fx, &fy)) {
            continue;
        }
        bool favorite = contact_find(n->pub_key) >= 0;
        draw_role_pin(fx, fy, n->role, favorite);

        int dx = fx - chx;
        int dy = fy - chy;
        int d2 = dx * dx + dy * dy;
        if (d2 < best_d2) {
            best_d2 = d2;
            best_fx = fx;
            best_fy = fy;
            strncpy(best_name, n->name, CONTACT_ALIAS_LEN);
            best_name[CONTACT_ALIAS_LEN] = 0;
        }
    }
    pax_noclip(&fb);
    xSemaphoreGive(node_mutex);

    // Name label for the node closest to the crosshair — only when there's
    // a node within ~80 px so panning to empty regions doesn't surface a
    // confusingly distant label.
    if (best_name[0] && best_d2 < 80 * 80) {
        pax_vec2f nsz = pax_text_size(FONT, TXT_SMALL, best_name);
        int       lx  = best_fx - (int)nsz.x / 2;
        int       ly  = best_fy + 8;
        int       pad = 3;
        pax_simple_rect(&fb, 0xC0000000, lx - pad, ly - pad, (int)nsz.x + 2 * pad, TXT_SMALL + 2 * pad);
        pax_draw_text(&fb, COL_WHITE, FONT, TXT_SMALL, lx, ly, best_name);
    }
}

void render_map(void) {
    int w = (int)pax_buf_get_width(&fb);
    int h = (int)pax_buf_get_height(&fb);

    pax_background(&fb, COL_BG);

    // Lock-to-position: snap centre to the latest GPS fix before drawing
    // tiles, so the rest of the frame uses the corrected centre.
    if (map_lock_on && gps_live_valid) {
        map_center_lat_e6 = gps_live_lat_e6;
        map_center_lon_e6 = gps_live_lon_e6;
    }

    render_map_header(w);

    int map_x0 = 0;
    int map_y0 = MAP_HEADER_H;
    int map_w  = w;
    int map_h  = h - MAP_HEADER_H - MAP_FOOTER_H;

    double lat = (double)map_center_lat_e6 / 1e6;
    double lon = (double)map_center_lon_e6 / 1e6;
    int    z   = (int)map_zoom;

    render_tile_raster(lat, lon, z, map_x0, map_y0, map_w, map_h);
    render_node_pins(lat, lon, z, map_x0, map_y0, map_w, map_h);
    render_crosshair(map_x0, map_y0, map_w, map_h);
    render_scale_bar(lat, z, map_x0, map_y0, map_w, map_h);
    render_legend(w, map_y0);
    render_status_strip(w, map_y0);
    render_map_attribution(w, h);

    // First-fix toast — one-shot per VIEW_MAP visit.
    if (!s_first_fix_seen && gps_live_valid) {
        s_first_fix_seen = true;
        snprintf(toast_text, sizeof(toast_text), "GPS fix locked");
        toast_start_ms    = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        toast_duration_ms = 2000;
    }

    // Persist the centre + zoom once the user pauses for MAP_STATE_DEBOUNCE_MS.
    map_state_tick();
}
