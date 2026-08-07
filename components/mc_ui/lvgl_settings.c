// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// VIEW_SETTINGS and the category glyphs.

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

// ── VIEW_SETTINGS ────────────────────────────────────────────────────────────
// Pixel-matched port of settings_fields.c (category-list grid + drilldown) and
// the category glyphs from the former render_settings_icons.c. This view reuses
// the field registry (settings_field_label/value) and the category table
// helpers so no per-field logic is duplicated here.

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

// WiFi: a fan of concentric signal arcs over a dot, opening upward.
void cat_icon_wifi_lv(lv_obj_t* s, int cx, int cy, int sz, uint32_t col) {
    int   half = sz / 2;
    int   oy   = cy + half / 3;  // arc origin sits low so the fan opens up
    float pi   = 3.14159265f;
    // Three broad arcs fanning upward over a dot (the standard WiFi glyph):
    // span ~9 o'clock .. 3 o'clock through the top so each wave is nice and wide.
    add_arc_rad(s, cx, oy, half * 9 / 10, -pi * 11.0f / 12.0f, -pi / 12.0f, 2, col);
    add_arc_rad(s, cx, oy, half * 6 / 10, -pi * 11.0f / 12.0f, -pi / 12.0f, 2, col);
    add_arc_rad(s, cx, oy, half * 3 / 10, -pi * 11.0f / 12.0f, -pi / 12.0f, 2, col);
    add_circle(s, cx, oy, sz / 12, col, -1, 0);
}

// Bluetooth: the runic glyph (vertical spine + two crossed bowties).
void cat_icon_bluetooth_lv(lv_obj_t* s, int cx, int cy, int sz, uint32_t col) {
    int half = sz / 2;
    int top  = cy - half;
    int bot  = cy + half;
    int rx   = half / 2;  // horizontal reach of the bowtie tips
    int qy   = cy - half / 2;
    int by   = cy + half / 2;
    // Spine from the top apex to the bottom apex.
    add_line(s, cx, top, cx, bot, 2, col);
    // Upper triangle: apex top -> right at upper-quarter -> centre.
    add_line(s, cx, top, cx + rx, qy, 2, col);
    add_line(s, cx + rx, qy, cx - rx, by, 2, col);
    // Lower triangle: centre -> right at lower-quarter -> apex bottom.
    add_line(s, cx, bot, cx + rx, by, 2, col);
    add_line(s, cx + rx, by, cx - rx, qy, 2, col);
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

void cat_icon_toolbox_lv(lv_obj_t* s, int cx, int cy, int sz, uint32_t col) {
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
// Index order MUST match s_categories[] in settings_fields.c (real index,
// including the hidden-from-grid Advert slot).
static const cat_icon_lv_fn s_cat_icons_lv[] = {
    cat_icon_identity_lv,  cat_icon_regulatory_lv, cat_icon_radio_lv,      cat_icon_advert_lv, cat_icon_wifi_lv,
    cat_icon_bluetooth_lv, cat_icon_region_lv,     cat_icon_brightness_lv, cat_icon_sounds_lv, cat_icon_toolbox_lv,
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
                              f == FIELD_GPS_LAT || f == FIELD_GPS_LON || f == FIELD_BLE_PIN);
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

void render_settings_lvgl(void) {
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
