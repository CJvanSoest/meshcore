// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// VIEW_MAP.

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "app_config.h"
#include "appfs.h"
#include "backup.h"
#include "ble_companion.h"
#include "bsp/power.h"
#include "channels.h"
#include "chat.h"
#include "contacts.h"
#include "coverage.h"
#include "diag.h"
#include "diag_decode.h"
#include "emoji_table.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gps_task.h"
#include "history.h"
#include "identity.h"
#include "locfs.h"
#include "lvgl.h"
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

// Shared status toast, pixel-matched to render.c::render_toast. Used by the map
// first-fix latch and the packet-log SD-export confirmation.
void status_toast_lvgl(lv_obj_t* scr, int w, int h) {
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

void render_map_lvgl(void) {
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
                int             tile_x  = center_tx + dx;
                int             tile_y  = center_ty + dy;
                int             tx      = origin_x + dx * MAP_TILE_PX;
                int             ty      = origin_y + dy * MAP_TILE_PX;
                bool            visible = !(tx + MAP_TILE_PX < 0 || tx >= map_w || ty + MAP_TILE_PX < 0 || ty >= map_h);
                // Request every cell (visible + ring) so a one-step pan finds
                // the new edge tiles already warm in the LRU cache.
                const uint16_t* t       = map_tile_get(z, tile_x, tile_y);
                if (!visible || !t || img_n >= (int)(sizeof(imgs) / sizeof(imgs[0]))) continue;
                lv_image_dsc_t* img = &imgs[img_n++];
                memset(img, 0, sizeof(*img));
                img->header.magic  = LV_IMAGE_HEADER_MAGIC;
                img->header.cf     = LV_COLOR_FORMAT_RGB565;
                img->header.w      = MAP_TILE_PX;
                img->header.h      = MAP_TILE_PX;
                img->header.stride = MAP_TILE_PX * 2;
                img->data_size     = MAP_TILE_PX * MAP_TILE_PX * 2;
                img->data          = (const uint8_t*)t;
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
    status_toast_lvgl(scr, w, h);

    // Persist centre + zoom once the user pauses (debounced NVS write).
    map_state_tick();
}
