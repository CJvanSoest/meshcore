// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "render.h"
#include "render_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"

#include "app_config.h"
#include "identity.h"
#include "nodes.h"
#include "radio.h"
#include "radio_system_protocol_client.h"
#include "region_limits.h"
#include "settings_nvs.h"
#include "ui_state.h"

extern bool c6_available;

// ── Settings category table ──────────────────────────────────────────────────
// Single source of truth for the drilldown. `first` is the enum index where
// the category begins; `last` is derived from the next category's `first`
// (or FIELD_COUNT - 1 for the tail). The titles are shown both in the
// category-list and as the drilled-in view header.
typedef struct {
    field_t     first;
    const char *title;
    const char *subtitle;
} settings_category_t;

static const settings_category_t s_categories[] = {
    { FIELD_RADIO_FW,     "Identity",          "Owner name, advert name, radio firmware"      },
    { FIELD_COUNTRY,      "Regulatory",        "Country, antenna gain, duty cycle"            },
    { FIELD_FREQ,         "Radio",             "Freq, SF, BW, CR, power, sync, preamble, ..." },
    { FIELD_ADVERT_INT,   "Network",           "Advert interval, role, path hash"             },
    { FIELD_REGION_SCOPE, "Region &\nLocation","Region scope, GPS coordinates"                },
    { FIELD_DISPLAY_BL,   "Brightness",        "Display, keyboard, RGB LED, auto-blank"       },
};
#define S_CATEGORY_COUNT ((int)(sizeof(s_categories) / sizeof(s_categories[0])))

int settings_category_count(void) { return S_CATEGORY_COUNT; }

void settings_category_bounds(int cat, int *first_field, int *last_field) {
    if (cat < 0 || cat >= S_CATEGORY_COUNT) {
        *first_field = 0;
        *last_field  = FIELD_COUNT - 1;
        return;
    }
    *first_field = (int)s_categories[cat].first;
    *last_field  = (cat + 1 < S_CATEGORY_COUNT)
                   ? (int)s_categories[cat + 1].first - 1
                   : FIELD_COUNT - 1;
}

const char *settings_category_title(int cat) {
    if (cat < 0 || cat >= S_CATEGORY_COUNT) return "Settings";
    return s_categories[cat].title;
}

int settings_category_for_field(int f) {
    for (int c = S_CATEGORY_COUNT - 1; c >= 0; c--) {
        if (f >= (int)s_categories[c].first) return c;
    }
    return 0;
}

// ── Tile-grid icons for the category screen ──────────────────────────────────
// Drawn at the centre of each tile in the Pager-style category grid; same
// primitive style as render_home.c so the two screens read as siblings.
static void cat_icon_identity(int cx, int cy, int sz, pax_col_t col) {
    pax_outline_circle(&fb, col, cx, cy - sz / 6, sz / 4);  // head
    pax_outline_circle(&fb, col, cx, cy + sz / 3, sz / 2);  // shoulders (cropped)
}

static void cat_icon_regulatory(int cx, int cy, int sz, pax_col_t col) {
    int s = sz / 2;
    pax_simple_line(&fb, col, cx,     cy - s, cx + s, cy - s / 3);
    pax_simple_line(&fb, col, cx + s, cy - s / 3, cx + s, cy + s / 3);
    pax_simple_line(&fb, col, cx + s, cy + s / 3, cx,     cy + s);
    pax_simple_line(&fb, col, cx,     cy + s, cx - s, cy + s / 3);
    pax_simple_line(&fb, col, cx - s, cy + s / 3, cx - s, cy - s / 3);
    pax_simple_line(&fb, col, cx - s, cy - s / 3, cx,     cy - s);
}

static void cat_icon_radio(int cx, int cy, int sz, pax_col_t col) {
    int q = sz / 2;
    pax_outline_circle(&fb, col, cx, cy, q / 4);  // node
    pax_outline_hollow_circle(&fb, col, cx, cy, q / 2 - 2, q / 2);
    pax_outline_hollow_circle(&fb, col, cx, cy, q     - 2, q);
}

static void cat_icon_network(int cx, int cy, int sz, pax_col_t col) {
    int s = sz / 3;
    pax_simple_circle(&fb, col, cx,      cy - s, sz / 12);
    pax_simple_circle(&fb, col, cx - s,  cy + s, sz / 12);
    pax_simple_circle(&fb, col, cx + s,  cy + s, sz / 12);
    pax_simple_line  (&fb, col, cx,      cy - s, cx - s, cy + s);
    pax_simple_line  (&fb, col, cx,      cy - s, cx + s, cy + s);
    pax_simple_line  (&fb, col, cx - s,  cy + s, cx + s, cy + s);
}

static void cat_icon_region(int cx, int cy, int sz, pax_col_t col) {
    // Map-pin-ish: tear-drop outline + small dot in the bell.
    int r = sz / 3;
    pax_outline_circle(&fb, col, cx, cy - r / 2, r);
    pax_simple_line(&fb, col, cx - r * 7 / 10, cy - r / 8, cx, cy + r * 5 / 4);
    pax_simple_line(&fb, col, cx + r * 7 / 10, cy - r / 8, cx, cy + r * 5 / 4);
    pax_simple_circle(&fb, col, cx, cy - r / 2, r / 3);
}

static void cat_icon_brightness(int cx, int cy, int sz, pax_col_t col) {
    int r = sz / 5;
    pax_simple_circle(&fb, col, cx, cy, r);
    // 8 rays evenly around the sun.
    for (int a = 0; a < 8; a++) {
        float t  = (float)a * 3.14159f / 4.0f;
        float r0 = (float)r + sz / 14.0f;
        float r1 = (float)sz / 2.0f;
        pax_simple_line(&fb, col,
                        cx + r0 * cosf(t), cy + r0 * sinf(t),
                        cx + r1 * cosf(t), cy + r1 * sinf(t));
    }
}

typedef void (*cat_icon_fn)(int cx, int cy, int sz, pax_col_t col);
static const cat_icon_fn s_category_icons[] = {
    cat_icon_identity,
    cat_icon_regulatory,
    cat_icon_radio,
    cat_icon_network,
    cat_icon_region,
    cat_icon_brightness,
};

// ── Category-list renderer ───────────────────────────────────────────────────
// 4-column Pager-style tile grid (same proportions as render_home.c), one
// tile per category. Empty cells in the grid stay blank.
#define S_GRID_COLS     4
#define S_GRID_H_MARG  30
#define S_GRID_V_MARG  20
#define S_GRID_FOOTER  38

static void render_settings_category_list(int w, int h) {
    int area_y0 = TAB_BAR_H + S_GRID_V_MARG;
    int area_h  = h - area_y0 - S_GRID_V_MARG - S_GRID_FOOTER;
    int area_w  = w - S_GRID_H_MARG * 2;

    int rows    = (S_CATEGORY_COUNT + S_GRID_COLS - 1) / S_GRID_COLS;
    if (rows < 1) rows = 1;

    int tile_w  = (area_w - S_GRID_H_MARG * (S_GRID_COLS - 1)) / S_GRID_COLS;
    int tile_h  = (area_h - S_GRID_V_MARG * (rows - 1)) / rows;

    for (int i = 0; i < S_CATEGORY_COUNT; i++) {
        int col = i % S_GRID_COLS;
        int row = i / S_GRID_COLS;
        int tx  = S_GRID_H_MARG + col * (tile_w + S_GRID_H_MARG);
        int ty  = area_y0       + row * (tile_h + S_GRID_V_MARG);

        bool focused  = (i == settings_category_cursor);
        pax_col_t bg  = focused ? COL_PAGER_ACCENT : COL_PAGER_TILE;
        pax_col_t fg  = focused ? COL_HEADER       : COL_PAGER_TEXT;

        pax_simple_rect(&fb, bg, tx, ty, tile_w, tile_h);
        if (focused) {
            pax_simple_rect(&fb, COL_PAGER_BG, tx + 2, ty + 2, tile_w - 4, 2);
            pax_simple_rect(&fb, COL_PAGER_BG, tx + 2, ty + tile_h - 4, tile_w - 4, 2);
            pax_simple_rect(&fb, COL_PAGER_BG, tx + 2, ty + 2, 2, tile_h - 4);
            pax_simple_rect(&fb, COL_PAGER_BG, tx + tile_w - 4, ty + 2, 2, tile_h - 4);
        }

        int icon_sz = tile_w / 2;
        if (icon_sz > tile_h * 2 / 5) icon_sz = tile_h * 2 / 5;
        int icon_cx = tx + tile_w / 2;
        int icon_cy = ty + tile_h * 2 / 5;
        if (i < (int)(sizeof(s_category_icons) / sizeof(s_category_icons[0])) &&
            s_category_icons[i]) {
            s_category_icons[i](icon_cx, icon_cy, icon_sz, fg);
        }

        // Render label centered; support an embedded '\n' so labels that
        // overflow a tile (e.g. "Region &\nLocation") wrap onto a second
        // line instead of being clipped.
        const char *lbl = s_categories[i].title;
        const char *nl  = strchr(lbl, '\n');
        int ly_base = ty + tile_h * 3 / 4;
        if (nl) {
            char line1[40];
            int  n1 = nl - lbl;
            if (n1 >= (int)sizeof(line1)) n1 = sizeof(line1) - 1;
            memcpy(line1, lbl, n1);
            line1[n1] = '\0';
            const char *line2 = nl + 1;
            pax_vec2f sz1 = pax_text_size(FONT, TXT_BODY, line1);
            pax_vec2f sz2 = pax_text_size(FONT, TXT_BODY, line2);
            int ly1 = ly_base - (TXT_BODY / 2 + 2);
            int ly2 = ly_base + (TXT_BODY / 2 + 2);
            pax_draw_text(&fb, fg, FONT, TXT_BODY,
                          tx + (tile_w - (int)sz1.x) / 2, ly1, line1);
            pax_draw_text(&fb, fg, FONT, TXT_BODY,
                          tx + (tile_w - (int)sz2.x) / 2, ly2, line2);
        } else {
            pax_vec2f lsz = pax_text_size(FONT, TXT_BODY, lbl);
            pax_draw_text(&fb, fg, FONT, TXT_BODY,
                          tx + (tile_w - (int)lsz.x) / 2, ly_base, lbl);
        }
    }

    int fy = h - S_GRID_FOOTER;
    pax_simple_rect(&fb, COL_HEADER,       0, fy, w, S_GRID_FOOTER);
    pax_simple_rect(&fb, COL_PAGER_ACCENT, 0, fy, w, 1);
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 10,
                  fy + (S_GRID_FOOTER - TXT_SMALL) / 2,
                  "WSAD: nav   Enter: open   ESC: home   Tab: next view");
}

// ── Drilled-in category renderer ─────────────────────────────────────────────
// Reuses the original row-rendering logic but only walks the fields belonging
// to settings_category_active, so headings + scroll math collapse to a flat
// list capped at ~9 rows (current max is "Radio" with 9 fields).
static void render_settings_drilldown(int w, int h) {
    int first_field, last_field;
    settings_category_bounds(settings_category_active, &first_field, &last_field);
    int field_count_local = last_field - first_field + 1;

    if (edit_mode) {
        const char *mode_str = "[EDIT]";
        pax_vec2f sz = pax_text_size(FONT, TXT_SMALL, mode_str);
        pax_draw_text(&fb, COL_AMBER, FONT, TXT_SMALL, w - (int)sz.x - 110,
                      (TAB_BAR_H - TXT_SMALL) / 2, mode_str);
    }

    typedef struct { const char *label; char value[64]; } row_t;
    row_t rows[FIELD_COUNT];

    rows[FIELD_RADIO_FW].label = "Radio ID";
    snprintf(rows[FIELD_RADIO_FW].value, sizeof(rows[FIELD_RADIO_FW].value), "%s",
             radio_fw_version[0] ? radio_fw_version : "?");

    rows[FIELD_RADIO_FW_APP].label = "Radio firmware";
    snprintf(rows[FIELD_RADIO_FW_APP].value, sizeof(rows[FIELD_RADIO_FW_APP].value),
             "%s", radio_fw_app_version[0] ? radio_fw_app_version : TANMATSU_RADIO_FW_LABEL);

    snprintf(rows[FIELD_OWNER].value, sizeof(rows[FIELD_OWNER].value), "%s", owner_name);
    rows[FIELD_OWNER].label = "Owner name";

    rows[FIELD_ADV_NAME].label = "Advert name";
    snprintf(rows[FIELD_ADV_NAME].value, sizeof(rows[FIELD_ADV_NAME].value), "%s",
             lora_advert_name[0] ? lora_advert_name : "(use owner)");

    rows[FIELD_COUNTRY].label = "Country";
    {
        const regulatory_country_t *rc = region_get_country(country_code);
        if (!rc || rc->n_subbands == 0) {
            snprintf(rows[FIELD_COUNTRY].value, sizeof(rows[FIELD_COUNTRY].value),
                     "(not set)");
        } else {
            const regulatory_subband_t *sb = region_match_subband(
                rc, (float)lora_cfg.frequency / 1000000.0f);
            if (sb) {
                snprintf(rows[FIELD_COUNTRY].value, sizeof(rows[FIELD_COUNTRY].value),
                         "%s [%s]", rc->display_name, sb->label);
            } else {
                snprintf(rows[FIELD_COUNTRY].value, sizeof(rows[FIELD_COUNTRY].value),
                         "%s [!off-band]", rc->display_name);
            }
        }
    }

    rows[FIELD_ANTENNA_GAIN].label = "Antenna gain";
    if (country_code[0] == '-' || country_code[0] == '\0') {
        snprintf(rows[FIELD_ANTENNA_GAIN].value, sizeof(rows[FIELD_ANTENNA_GAIN].value),
                 "(set country)");
    } else {
        snprintf(rows[FIELD_ANTENNA_GAIN].value, sizeof(rows[FIELD_ANTENNA_GAIN].value),
                 "%d dBi", (int)antenna_gain_dbi);
    }

    rows[FIELD_FREQ].label = "Frequency";
    snprintf(rows[FIELD_FREQ].value, sizeof(rows[FIELD_FREQ].value),
             "%.3f MHz", (double)lora_cfg.frequency / 1000000.0);

    rows[FIELD_SF].label = "Spreading factor";
    snprintf(rows[FIELD_SF].value, sizeof(rows[FIELD_SF].value), "SF%d", lora_cfg.spreading_factor);

    rows[FIELD_BW].label = "Bandwidth";
    snprintf(rows[FIELD_BW].value, sizeof(rows[FIELD_BW].value), "%d kHz", (int)lora_cfg.bandwidth);

    rows[FIELD_CR].label = "Coding rate";
    snprintf(rows[FIELD_CR].value, sizeof(rows[FIELD_CR].value), "4/%d", lora_cfg.coding_rate);

    rows[FIELD_POWER].label = "TX power";
    snprintf(rows[FIELD_POWER].value, sizeof(rows[FIELD_POWER].value), "%d dBm", (int)lora_cfg.power);

    rows[FIELD_SYNC].label = "Sync word";
    snprintf(rows[FIELD_SYNC].value, sizeof(rows[FIELD_SYNC].value), "0x%02X", (unsigned)lora_cfg.sync_word);

    rows[FIELD_PREAMBLE].label = "Preamble length";
    snprintf(rows[FIELD_PREAMBLE].value, sizeof(rows[FIELD_PREAMBLE].value), "%d", (int)lora_cfg.preamble_length);

    rows[FIELD_ADVERT_INT].label = "Advert interval";
    if (advert_interval_s < 60) {
        snprintf(rows[FIELD_ADVERT_INT].value, sizeof(rows[FIELD_ADVERT_INT].value), "%us", (unsigned)advert_interval_s);
    } else {
        snprintf(rows[FIELD_ADVERT_INT].value, sizeof(rows[FIELD_ADVERT_INT].value), "%umin", (unsigned)(advert_interval_s / 60));
    }

    rows[FIELD_PRESET].label = "LoRa preset";
    {
        int pidx = lora_preset_match();
        if (pidx >= 0) {
            snprintf(rows[FIELD_PRESET].value, sizeof(rows[FIELD_PRESET].value), "%s", LORA_PRESETS[pidx].name);
        } else {
            snprintf(rows[FIELD_PRESET].value, sizeof(rows[FIELD_PRESET].value), "(custom)");
        }
    }

    rows[FIELD_ROLE].label = "Role";
    snprintf(rows[FIELD_ROLE].value, sizeof(rows[FIELD_ROLE].value), "%s", role_label(lora_role));

    rows[FIELD_PATH_HASH_SIZE].label = "Path hash size";
    {
        static const char *hops[] = {"64 hops", "32 hops", "21 hops"};
        int hi = (path_hash_size >= 1 && path_hash_size <= 3) ? (path_hash_size - 1) : 0;
        snprintf(rows[FIELD_PATH_HASH_SIZE].value, sizeof(rows[FIELD_PATH_HASH_SIZE].value),
                 "%u byte (%s)", path_hash_size, hops[hi]);
    }

    rows[FIELD_SENSITIVITY].label = "RX sensitivity";
    snprintf(rows[FIELD_SENSITIVITY].value, sizeof(rows[FIELD_SENSITIVITY].value),
             "%s", lora_cfg.rx_boost ? "High (+3dB)" : "Power save");

    rows[FIELD_REGION_SCOPE].label = "Region scope";
    snprintf(rows[FIELD_REGION_SCOPE].value, sizeof(rows[FIELD_REGION_SCOPE].value),
             "%s", region_scope[0] ? region_scope : "(not set)");

    rows[FIELD_GPS_LAT].label = "GPS latitude";
    if (gps_position_valid) {
        snprintf(rows[FIELD_GPS_LAT].value, sizeof(rows[FIELD_GPS_LAT].value),
                 "%.6f", (double)gps_lat_e6 / 1e6);
    } else {
        snprintf(rows[FIELD_GPS_LAT].value, sizeof(rows[FIELD_GPS_LAT].value), "(not set)");
    }
    rows[FIELD_GPS_LON].label = "GPS longitude";
    if (gps_position_valid) {
        snprintf(rows[FIELD_GPS_LON].value, sizeof(rows[FIELD_GPS_LON].value),
                 "%.6f", (double)gps_lon_e6 / 1e6);
    } else {
        snprintf(rows[FIELD_GPS_LON].value, sizeof(rows[FIELD_GPS_LON].value), "(not set)");
    }

    rows[FIELD_DUTY_CYCLE].label = "Duty cycle (1h)";
    if (dc_budget_ms == 0 || dc_budget_ms >= 3600000u) {
        snprintf(rows[FIELD_DUTY_CYCLE].value, sizeof(rows[FIELD_DUTY_CYCLE].value),
                 "%lus used (no limit)", (unsigned long)(dc_used_ms / 1000u));
    } else {
        unsigned pct_x10 = (unsigned)(((uint64_t)dc_used_ms * 1000u) / dc_budget_ms);
        snprintf(rows[FIELD_DUTY_CYCLE].value, sizeof(rows[FIELD_DUTY_CYCLE].value),
                 "%u.%u%% (%lus / %lus)%s",
                 pct_x10 / 10u, pct_x10 % 10u,
                 (unsigned long)(dc_used_ms / 1000u),
                 (unsigned long)(dc_budget_ms / 1000u),
                 dc_last_tx_blocked ? " BLOCKED" : "");
    }

    rows[FIELD_DISPLAY_BL].label = "Display backlight";
    snprintf(rows[FIELD_DISPLAY_BL].value, sizeof(rows[FIELD_DISPLAY_BL].value),
             "%u%%", (unsigned)display_brightness);
    rows[FIELD_KB_BL].label = "Keyboard backlight";
    snprintf(rows[FIELD_KB_BL].value, sizeof(rows[FIELD_KB_BL].value),
             "%u%%", (unsigned)keyboard_brightness);
    rows[FIELD_LED_BR].label = "RGB LED brightness";
    snprintf(rows[FIELD_LED_BR].value, sizeof(rows[FIELD_LED_BR].value),
             "%u%%", (unsigned)led_brightness);
    rows[FIELD_BLANK_AFTER].label = "Auto-blank display";
    if (display_blank_after_s == 0) {
        snprintf(rows[FIELD_BLANK_AFTER].value, sizeof(rows[FIELD_BLANK_AFTER].value), "Off");
    } else if (display_blank_after_s < 60) {
        snprintf(rows[FIELD_BLANK_AFTER].value, sizeof(rows[FIELD_BLANK_AFTER].value),
                 "%us", (unsigned)display_blank_after_s);
    } else {
        snprintf(rows[FIELD_BLANK_AFTER].value, sizeof(rows[FIELD_BLANK_AFTER].value),
                 "%umin", (unsigned)(display_blank_after_s / 60));
    }

    const int row_h    = 44;
    const int title_h  = 38;
    const int footer_h = 60;
    const int y0       = TAB_BAR_H + 6;

    pax_draw_text(&fb, COL_AMBER, FONT, TXT_TITLE, 18, y0,
                  settings_category_title(settings_category_active));
    pax_simple_rect(&fb, COL_AMBER, 18, y0 + TXT_TITLE + 4, w - 36, 1);

    int list_y0 = y0 + title_h;
    int list_h  = h - list_y0 - footer_h;

    if (selected < first_field) selected = first_field;
    if (selected > last_field)  selected = last_field;

    int sel_row = selected - first_field;
    int reveal_top = sel_row * row_h;
    int reveal_bot = reveal_top + row_h;
    if (reveal_top < settings_scroll)          settings_scroll = reveal_top;
    if (reveal_bot > settings_scroll + list_h) settings_scroll = reveal_bot - list_h;
    int max_scroll = field_count_local * row_h - list_h;
    if (max_scroll < 0)               max_scroll = 0;
    if (settings_scroll > max_scroll) settings_scroll = max_scroll;
    if (settings_scroll < 0)          settings_scroll = 0;

    int text_y_off = (row_h - TXT_BODY) / 2;

    pax_clip(&fb, 0, list_y0, w, list_h);
    for (int f = first_field; f <= last_field; f++) {
        int rel = f - first_field;
        int y   = list_y0 + rel * row_h - settings_scroll;
        if (y + row_h <= list_y0 || y >= list_y0 + list_h) continue;

        bool is_sel = (f == selected);
        if (is_sel) {
            pax_col_t bg  = edit_mode ? 0xFF3A2A1A : COL_PANEL;
            pax_col_t bar = edit_mode ? COL_AMBER  : COL_ACCENT;
            pax_simple_rect(&fb, bg,  0, y, w, row_h - 1);
            pax_simple_rect(&fb, bar, 0, y, 5, row_h - 1);
        }
        pax_simple_rect(&fb, COL_PANEL, 12, y + row_h - 1, w - 24, 1);

        pax_col_t lbl_col = is_sel ? COL_WHITE : COL_GRAY;
        pax_draw_text(&fb, lbl_col, FONT, TXT_BODY, 18, y + text_y_off, rows[f].label);

        pax_col_t val_col;
        bool regulatory_violation = false;
        if (f == FIELD_FREQ || f == FIELD_POWER || f == FIELD_COUNTRY) {
            const regulatory_country_t *rc = region_get_country(country_code);
            if (rc && rc->n_subbands > 0) {
                const regulatory_subband_t *sb = region_match_subband(
                    rc, (float)lora_cfg.frequency / 1000000.0f);
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

        char val_disp[80];
        bool is_text_field = (f == FIELD_OWNER || f == FIELD_ADV_NAME ||
                              f == FIELD_REGION_SCOPE ||
                              f == FIELD_GPS_LAT || f == FIELD_GPS_LON);
        if (is_sel && edit_mode && field_editing_text && is_text_field) {
            snprintf(val_disp, sizeof(val_disp), "%s_", field_edit_buf);
        } else if (is_sel && edit_mode && !is_text_field) {
            snprintf(val_disp, sizeof(val_disp), "< %s >", rows[f].value);
        } else {
            snprintf(val_disp, sizeof(val_disp), "%s", rows[f].value);
        }
        pax_vec2f vsz = pax_text_size(FONT, TXT_BODY, val_disp);
        pax_draw_text(&fb, val_col, FONT, TXT_BODY, w - (int)vsz.x - 18, y + text_y_off, val_disp);
    }
    pax_noclip(&fb);

    int fy = h - footer_h;
    pax_simple_rect(&fb, COL_HEADER, 0, fy, w, footer_h);
    pax_simple_rect(&fb, COL_PANEL, 0, fy, w, 1);

    const char *hint = NULL;
    char        hintbuf[128];
    pax_col_t   hint_col = COL_GRAY;
    if (edit_mode && field_editing_text) {
        hint = "Type to edit   Backspace: del   Enter: save   ESC: cancel";
    } else if (edit_mode) {
        hint = "Up/Down or W/S: adjust   Enter: save   ESC: cancel";
    } else if (!c6_available) {
        hint = "NVS only — update radio via Launcher: Tools > Firmware update";
        hint_col = COL_AMBER;
    } else if (selected == FIELD_ANTENNA_GAIN) {
        hint = "Antenna gain raises ERP; editable once Country is set.";
    } else if (selected == FIELD_COUNTRY || selected == FIELD_FREQ ||
               selected == FIELD_POWER   || selected == FIELD_DUTY_CYCLE) {
        const regulatory_country_t *rc = region_get_country(country_code);
        if (!rc || rc->n_subbands == 0) {
            hint = "Set Country to see allowed band / power / duty-cycle limits.";
        } else {
            const regulatory_subband_t *sb =
                region_match_subband(rc, (float)lora_cfg.frequency / 1000000.0f);
            const char *unit = (rc->power_unit == POWER_UNIT_EIRP) ? "EIRP" : "ERP";
            if (!sb) {
                snprintf(hintbuf, sizeof(hintbuf),
                         "%s: %.3f MHz off-band — pick a frequency in an allowed sub-band.",
                         rc->display_name, (double)lora_cfg.frequency / 1e6);
            } else {
                snprintf(hintbuf, sizeof(hintbuf),
                         "%s %s: %.2f-%.2f MHz, max %d dBm %s, %u.%u%% duty cycle.",
                         rc->display_name, sb->label,
                         (double)sb->freq_min_mhz, (double)sb->freq_max_mhz,
                         (int)sb->max_power_dbm, unit,
                         sb->duty_cycle_permille / 10u, sb->duty_cycle_permille % 10u);
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
    } else if (selected == FIELD_ADVERT_INT) {
        hint = "Advert interval: longer = lower duty cycle, saves battery";
    } else if (selected == FIELD_PRESET) {
        hint = "Preset overwrites SF/BW/CR. MeshCore = default net.";
    } else if (selected == FIELD_ROLE) {
        hint = "Role: advertised only. Does NOT enable repeater behavior.";
    } else if (selected == FIELD_GPS_LAT || selected == FIELD_GPS_LON) {
        hint = "Decimal degrees (e.g. 52.123456). Empty clears both axes.";
    } else if (selected == FIELD_DISPLAY_BL || selected == FIELD_KB_BL ||
               selected == FIELD_LED_BR    || selected == FIELD_BLANK_AFTER) {
        // Two-line layout in the existing 60-px footer:
        //   top: yellow-square icon hint (matches home screen)
        //   bot: standard nav hint (kept so users don't lose discovery)
        hint = NULL;
        int top_y = fy + 4;
        int bot_y = top_y + TXT_BODY + 4;
        const char *pre  = "Press ";
        const char *post = " to blank / wake display";
        pax_draw_text(&fb, hint_col, FONT, TXT_BODY, 10, top_y, pre);
        pax_vec2f pre_sz = pax_text_size(FONT, TXT_BODY, pre);
        int icon_sz = TXT_BODY - 6;
        int icon_x  = 10 + (int)pre_sz.x;
        int icon_y  = top_y + (TXT_BODY - icon_sz) / 2;
        pax_simple_rect(&fb, COL_YELLOW, icon_x, icon_y, icon_sz, icon_sz);
        pax_draw_text(&fb, hint_col, FONT, TXT_BODY,
                      icon_x + icon_sz + 4, top_y, post);
        pax_draw_text(&fb, hint_col, FONT, TXT_SMALL, 10, bot_y,
                      "W/S: navigate   Enter: edit   ESC: back   R: reload");
    } else {
        hint = "W/S: navigate   Enter: edit   ESC: back to categories   R: reload";
    }
    if (hint) {
        pax_draw_text(&fb, hint_col, FONT, TXT_BODY, 10, fy + 6, hint);
    }

    if (dirty) {
        const char *unsaved = "* unsaved";
        pax_vec2f usz = pax_text_size(FONT, TXT_BODY, unsaved);
        pax_draw_text(&fb, COL_AMBER, FONT, TXT_BODY, w - (int)usz.x - 10, fy + 6, unsaved);
    }
}

// ── Public entry point: dispatches between the category list and the
//   drilled-in field list based on settings_category_list_mode.
void render_settings(void) {
    int w = (int)pax_buf_get_width(&fb);
    int h = (int)pax_buf_get_height(&fb);

    pax_background(&fb, COL_BLACK);
    render_tab_bar();

    if (settings_category_list_mode) {
        render_settings_category_list(w, h);
    } else {
        render_settings_drilldown(w, h);
    }
}
