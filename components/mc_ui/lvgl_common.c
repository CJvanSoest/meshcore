// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// shared LVGL primitives, boot splash, vector glyphs and the tab bar.

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

// ── Shared LVGL helpers ──────────────────────────────────────────────────────

// COL_* are 0xAARRGGBB; LVGL wants RGB. Drop alpha.
inline lv_color_t mc_col(uint32_t argb) {
    return lv_color_make((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
}

// Map the app's TXT_* point sizes onto our extended Montserrat faces (ASCII +
// Latin-1 + euro/dashes) so umlauts and accents in received messages and the F5
// picker render real glyphs instead of the missing-glyph box. TXT_TINY (13) has
// no 13 px face; 14 is the closest.
static const lv_font_t* mc_font(int sz) {
    switch (sz) {
        case TXT_TINY:
            return &lv_font_montserrat_14_ext;
        case TXT_SMALL:
            return &lv_font_montserrat_16_ext;
        case TXT_BODY:
            return &lv_font_montserrat_20_ext;
        case TXT_TAB:
            return &lv_font_montserrat_22_ext;
        case TXT_TITLE:
            return &lv_font_montserrat_24_ext;
        default:
            return &lv_font_montserrat_16_ext;
    }
}

// Exact rendered width of `text` in the given size's face, used for centring
// and right-alignment.
int text_w(const char* text, int font_sz) {
    lv_point_t sz;
    lv_text_get_size(&sz, text, mc_font(font_sz), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return (int)sz.x;
}

// A flat label at absolute (x, y); y is the top of the text. No padding, no
// scroll, no wrap.
lv_obj_t* add_label(lv_obj_t* parent, int x, int y, int font_sz, uint32_t col, const char* text) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, mc_font(font_sz), 0);
    lv_obj_set_style_text_color(l, mc_col(col), 0);
    lv_obj_set_style_pad_all(l, 0, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

// Render `n` bytes as fixed-width hex columns for the Packet Log detail dump.
// Each "%02X" sits left-aligned in an equal-width cell, so rows line up
// vertically without a monospace font — the proportional Montserrat reads
// lighter (anti-aliased) than the chunky UNSCII bitmap face it replaces.
void add_hex_cols(lv_obj_t* parent, int x, int y, uint32_t col, const uint8_t* bytes, int n) {
    static int cell = 0;
    if (cell == 0) {
        cell = text_w("FF", TXT_SMALL) + 6;  // widest hex pair + a small gutter
    }
    for (int i = 0; i < n; i++) {
        char pair[3];
        snprintf(pair, sizeof(pair), "%02X", bytes[i]);
        add_label(parent, x + i * cell, y, TXT_SMALL, col, pair);
    }
}

// A filled rectangle: used for header/footer
// strips and accent lines.
lv_obj_t* add_rect(lv_obj_t* parent, int x, int y, int w, int h, uint32_t col) {
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
void add_back_hint(lv_obj_t* parent, int x, int y, const char* label, int ts) {
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
lv_obj_t* begin_screen(uint32_t bg_col) {
    lv_obj_t* scr = (lv_obj_t*)lvgl_port_screen();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, mc_col(bg_col), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    return scr;
}

// ── Boot splash ──────────────────────────────────────────────────────────────
// Incremental init readout shown during app_main, before the first view render.
// main.c stays LVGL-free: it calls these with COL_* ARGB values + plain text;
// each line is appended and flushed so the user sees init progress live.
// lvgl_splash_begin() clears the screen and paints the title and attribution;
// lvgl_splash_line() appends one status line below the previous and reflushes.
static int s_splash_y;

void lvgl_splash_begin(const char* title, const char* subtitle) {
    lv_obj_t* scr = begin_screen(COL_BG);
    add_label(scr, 14, 16, TXT_TITLE, COL_AMBER, title);
    add_label(scr, 14, 16 + TXT_TITLE + 4, TXT_SMALL, COL_AMBER, subtitle);
    s_splash_y = 74;  // below the title + attribution, matching the old PAX layout
    lvgl_port_refresh_now();
}

void lvgl_splash_line(uint32_t argb, const char* text) {
    lv_obj_t* scr = (lv_obj_t*)lvgl_port_screen();
    add_label(scr, 14, s_splash_y, TXT_SMALL, argb, text);
    s_splash_y += 22;
    lvgl_port_refresh_now();
}

// ── Vector-glyph primitives (for the Home tile icons) ────────────────────────
// lv_line keeps a pointer to the caller's point array, so the points must live
// until the frame is flushed. A per-frame pool reset at the top of each render
// supplies that storage; the screen is flushed synchronously before the next
// render reuses the pool.
#define PT_POOL 768
static lv_point_precise_t s_pt[PT_POOL];
static int                s_pt_n;

void pt_reset(void) {
    s_pt_n = 0;
}

void add_line(lv_obj_t* p, int x1, int y1, int x2, int y2, int w, uint32_t col) {
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
void add_circle(lv_obj_t* p, int cx, int cy, int r, int64_t fill, int64_t border, int bw) {
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

// A small "cloud" glyph — three overlapping lobes matching the blue F5 key
// symbol on the Tanmatsu (and the launcher's cyan cloud next to "Settings"). The
// footer affordance for the special-character picker, so the hint echoes the
// physical key the user presses.
void add_cloud(lv_obj_t* p, int cx, int cy, int r, uint32_t col) {
    // Three hollow rings in a trefoil (one up, two down), overlapping in the
    // centre — the exact shape printed on the blue F5 key. fill=-1 → outline only.
    int lobe = (r + 1) / 2;                                   // ring radius
    int off  = r / 2;                                         // spread from centre
    add_circle(p, cx, cy - off, lobe, -1, col, 1);            // top
    add_circle(p, cx - off, cy + off - 1, lobe, -1, col, 1);  // bottom-left
    add_circle(p, cx + off, cy + off - 1, lobe, -1, col, 1);  // bottom-right
}

// Static arc stroke from start_deg to end_deg (LVGL angles: 0 deg = 3 o'clock,
// increasing clockwise; wraps through 0 if start > end).
void add_arc(lv_obj_t* p, int cx, int cy, int r, int start_deg, int end_deg, int w, uint32_t col) {
    // Normalize to [0,360): lv_arc draws clockwise from start to end (wrapping
    // through 0 when start > end) but mishandles negative angles — without this
    // a wedge like -45..45 loses its top (-45..0) half.
    start_deg   = ((start_deg % 360) + 360) % 360;
    end_deg     = ((end_deg % 360) + 360) % 360;
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

// Shared status toast (defined with the map view, used by several views).

// ── Shared tab-bar header (classic views: Settings / Nodes / DM / Channel) ───
// Pixel-matched port of render_tab_bar() in render.c. Left: view name + inline
// DM / channel unread badges; right: RX | TX% | battery (reuses
// home_status_right, identical layout).

void tab_bar_lvgl(lv_obj_t* scr) {
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
