// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "app_config.h"
#include "cert_gen.h"
#include "gps.h"
#include "gps_task.h"
#include "http_server.h"
#include "map.h"
#include "nodes.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"
#include "radio.h"
#include "region_limits.h"
#include "render.h"
#include "render_internal.h"
#include "render_settings_icons.h"
#include "settings_nvs.h"
#include "sounds.h"
#include "ui_state.h"
#include "wifi_connection.h"

extern bool c6_available;

// ── Settings category table ──────────────────────────────────────────────────
// Single source of truth for the drilldown. `first` is the enum index where
// the category begins; `last` is derived from the next category's `first`
// (or FIELD_COUNT - 1 for the tail). The titles are shown both in the
// category-list and as the drilled-in view header.
typedef struct {
    field_t     first;
    const char* title;
    const char* subtitle;
    bool        hidden_from_grid;  // true = drilldown reachable, but tile not on the Settings grid
} settings_category_t;

// An "external" category opens a top-level view (the Toolbox launcher) instead
// of a field-list drilldown. It is marked by first == FIELD_COUNT so the
// bounds/lookup math never maps a real field to it; see settings_category_is_external.

static const settings_category_t s_categories[] = {
    {FIELD_RADIO_FW, "Identity", "Owner name, advert name, radio firmware", false},
    {FIELD_COUNTRY, "Regulatory", "Country, antenna gain, duty cycle", false},
    {FIELD_FREQ, "Radio", "Freq, SF, BW, CR, power, sync, preamble, ...", false},
    // Advert is reached only via the Home -> Advert tile (the tile drills in
    // directly). Hidden here so the Settings grid doesn't have a duplicate.
    {FIELD_FLOOD_ADVERT_INT, "Advert", "Flood + direct intervals, manual send", true},
    {FIELD_ROLE, "Network", "Role, path hash, WiFi, HTTP endpoint", false},
    {FIELD_REGION_SCOPE, "Region &\nLocation", "Region scope, GPS coordinates", false},
    {FIELD_DISPLAY_BL, "Brightness", "Display, keyboard, RGB LED, auto-blank", false},
    {FIELD_SOUND_VOLUME, "Sounds", "Volume + per-event toggles + previews", false},
    // External tile: opens the Toolbox launcher rather than a field drilldown.
    {FIELD_COUNT, "Toolbox", "Packet log + coverage test", false},
};
#define S_CATEGORY_COUNT ((int)(sizeof(s_categories) / sizeof(s_categories[0])))

// Visible categories = those without hidden_from_grid. Grid render + grid
// navigation work in visible-slot space; drilldown logic still uses the
// real s_categories index via settings_category_active. The Home -> Advert
// tile drives the drilldown directly so the hidden Advert category is
// still reachable.
static int s_visible_count(void) {
    int n = 0;
    for (int i = 0; i < S_CATEGORY_COUNT; i++)
        if (!s_categories[i].hidden_from_grid) n++;
    return n;
}
static int s_visible_real_idx(int slot) {
    int seen = 0;
    for (int i = 0; i < S_CATEGORY_COUNT; i++) {
        if (s_categories[i].hidden_from_grid) continue;
        if (seen == slot) return i;
        seen++;
    }
    return -1;
}
int settings_visible_category_count(void) {
    return s_visible_count();
}
int settings_visible_category_real_idx(int slot) {
    return s_visible_real_idx(slot);
}

int settings_category_count(void) {
    return S_CATEGORY_COUNT;
}

void settings_category_bounds(int cat, int* first_field, int* last_field) {
    if (cat < 0 || cat >= S_CATEGORY_COUNT) {
        *first_field = 0;
        *last_field  = FIELD_COUNT - 1;
        return;
    }
    *first_field = (int)s_categories[cat].first;
    *last_field  = (cat + 1 < S_CATEGORY_COUNT) ? (int)s_categories[cat + 1].first - 1 : FIELD_COUNT - 1;
}

const char* settings_category_title(int cat) {
    if (cat < 0 || cat >= S_CATEGORY_COUNT) return "Settings";
    return s_categories[cat].title;
}

bool settings_category_is_external(int cat, app_view_t* out_view) {
    if (cat < 0 || cat >= S_CATEGORY_COUNT) return false;
    if (s_categories[cat].first != FIELD_COUNT) return false;  // sentinel: no field range
    if (out_view) *out_view = VIEW_TOOLBOX;
    return true;
}

int settings_category_for_field(int f) {
    for (int c = S_CATEGORY_COUNT - 1; c >= 0; c--) {
        if (f >= (int)s_categories[c].first) return c;
    }
    return 0;
}

// ── Field registry (R6) ─────────────────────────────────────────────────────
// Single source of truth: per-field label + NVS-save callback. Replaces the
// earlier two parallel tables (per-category build_rows_X[] + input.c's
// s_field_savers[]) so adding a settings row is one designated-init entry
// here plus one case in fmt_field() below.
//
// `save = NULL` means: don't persist on edit-mode exit. Persistable fields
// without a dedicated save_*() fall through to save_lora_config() via
// field_save() — same fallback the old s_field_savers cascade had.
typedef struct {
    const char* label;
    void (*save)(void);
} field_def_t;

static const field_def_t s_fields[FIELD_COUNT] = {
    // ── Identity ──
    [FIELD_RADIO_FW]           = {"Radio ID", NULL},
    [FIELD_RADIO_FW_APP]       = {"Radio firmware", NULL},
    [FIELD_OWNER]              = {"Owner name", save_owner_name},
    [FIELD_ADV_NAME]           = {"Advert name", save_lora_advert_name},
    // ── Regulatory ──
    [FIELD_COUNTRY]            = {"Country", save_country_code},
    [FIELD_ANTENNA_GAIN]       = {"Antenna gain", save_antenna_gain},
    [FIELD_DUTY_CYCLE]         = {"Duty cycle (1h)", NULL},
    // ── Radio ──
    [FIELD_FREQ]               = {"Frequency", NULL},
    [FIELD_SF]                 = {"Spreading factor", NULL},
    [FIELD_BW]                 = {"Bandwidth", NULL},
    [FIELD_CR]                 = {"Coding rate", NULL},
    [FIELD_POWER]              = {"TX power", NULL},
    [FIELD_SYNC]               = {"Sync word", NULL},
    [FIELD_PREAMBLE]           = {"Preamble length", NULL},
    [FIELD_PRESET]             = {"LoRa preset", NULL},
    [FIELD_SENSITIVITY]        = {"RX sensitivity", NULL},
    // ── Advert ──
    [FIELD_FLOOD_ADVERT_INT]   = {"Flood interval", NULL},
    [FIELD_DIRECT_ADVERT_INT]  = {"Direct interval", NULL},
    [FIELD_SEND_FLOOD_NOW]     = {"Send flood now", NULL},
    [FIELD_SEND_DIRECT_NOW]    = {"Send direct now", NULL},
    // ── Network ──
    // Labels here are deliberately short; render_settings_drilldown draws
    // "WiFi" / "HTTPS server" section headers above the first row of each
    // group so the qualifier doesn't need to repeat on every label.
    [FIELD_ROLE]               = {"Role", NULL},
    [FIELD_PATH_HASH_SIZE]     = {"Path hash size", NULL},
    [FIELD_WIFI_SSID]          = {"SSID", NULL},
    [FIELD_WIFI_STATUS]        = {"Status", NULL},
    [FIELD_WIFI_NETWORK]       = {"Network", save_wifi_prefs},
    [FIELD_WIFI_ENABLED]       = {"Enabled", save_wifi_prefs},
    [FIELD_HTTP_URL]           = {"Endpoint", NULL},
    [FIELD_HTTP_API_KEY]       = {"API key", NULL},
    [FIELD_HTTP_KEY_REGEN]     = {"Regenerate key", NULL},
    [FIELD_HTTPS_CERT_FP]      = {"Cert fingerprint", NULL},
    [FIELD_HTTPS_CERT_REGEN]   = {"Regenerate cert", NULL},
    [FIELD_HTTP_QR]            = {"Show QR (OwnTracks)", NULL},
    // ── Region & Location ──
    [FIELD_REGION_SCOPE]       = {"Region scope", save_region_scope},
    [FIELD_GPS_LAT]            = {"GPS latitude", save_gps_coords},
    [FIELD_GPS_LON]            = {"GPS longitude", save_gps_coords},
    [FIELD_GPS_SOURCE]         = {"GPS source", NULL},
    [FIELD_GPS_AUTOFILL]       = {"Auto-fill from GPS", NULL},
    // ── Tracking (live GPS background task) ──
    [FIELD_GPS_PROFILE]        = {"Profile", save_gps_track_prefs},
    [FIELD_GPS_INTERVAL_S]     = {"Poll interval", save_gps_track_prefs},
    [FIELD_GPS_DISTANCE_M]     = {"Commit distance", save_gps_track_prefs},
    [FIELD_MAP_PROFILE]        = {"Style", save_map_profile},
    [FIELD_BLE_ENABLED]        = {"BLE companion", save_ble_enabled},
    // ── Brightness ──
    [FIELD_DISPLAY_BL]         = {"Display backlight", save_brightness},
    [FIELD_KB_BL]              = {"Keyboard backlight", save_brightness},
    [FIELD_LED_BR]             = {"RGB LED brightness", save_brightness},
    [FIELD_BLANK_AFTER]        = {"Auto-blank display", save_brightness},
    // ── Sounds ──
    [FIELD_SOUND_VOLUME]       = {"Volume", save_sound_prefs},
    [FIELD_SOUND_DM]           = {"DM sound", save_sound_prefs},
    [FIELD_SOUND_CHANNEL]      = {"Channel sound", save_sound_prefs},
    [FIELD_SOUND_ERROR]        = {"Error sound", save_sound_prefs},
    [FIELD_SOUND_BOOT]         = {"Boot sound", save_sound_prefs},
    [FIELD_SOUND_TEST_DM]      = {"Preview DM", NULL},
    [FIELD_SOUND_TEST_CHANNEL] = {"Preview channel", NULL},
    [FIELD_SOUND_TEST_ERROR]   = {"Preview error", NULL},
    [FIELD_SOUND_TEST_BOOT]    = {"Preview boot", NULL},
};

// Public save dispatch — replaces input.c's s_field_savers + persist_field_change.
// Fields without a dedicated save_*() fall back to save_lora_config(), which is
// the catch-all NVS-write for the radio/mesh registry.
void field_save(field_t f) {
    if (f < 0 || f >= FIELD_COUNT) return;
    if (s_fields[f].save)
        s_fields[f].save();
    else
        save_lora_config();
}

// row_t is the label/value pair rendered on one drilldown row. Label is
// copied from s_fields[f].label; value is produced by fmt_field() below.
typedef struct {
    const char* label;
    char        value[64];
} row_t;

// Forward decl — implementation lives near the bottom of this file so the
// switch sits next to the rendering code that consumes it.
static void fmt_field(field_t f, char* out, size_t cap);

// Category tile-grid icons live in render_settings_icons.c so this file
// stays focused on layout + per-field rendering. Index order in
// settings_category_icons[] must match s_categories[] above.

// ── Category-list renderer ───────────────────────────────────────────────────
// 4-column Pager-style tile grid (same proportions as render_home.c), one
// tile per category. Empty cells in the grid stay blank.
#define S_GRID_COLS   4
#define S_GRID_H_MARG 30
#define S_GRID_V_MARG 20
#define S_GRID_FOOTER 38

static void render_settings_category_list(int w, int h) {
    int area_y0 = TAB_BAR_H + S_GRID_V_MARG;
    int area_h  = h - area_y0 - S_GRID_V_MARG - S_GRID_FOOTER;
    int area_w  = w - S_GRID_H_MARG * 2;

    int visible = s_visible_count();
    int rows    = (visible + S_GRID_COLS - 1) / S_GRID_COLS;
    if (rows < 1) rows = 1;

    int tile_w = (area_w - S_GRID_H_MARG * (S_GRID_COLS - 1)) / S_GRID_COLS;
    int tile_h = (area_h - S_GRID_V_MARG * (rows - 1)) / rows;

    for (int slot = 0; slot < visible; slot++) {
        int i = s_visible_real_idx(slot);
        if (i < 0) continue;
        int col = slot % S_GRID_COLS;
        int row = slot / S_GRID_COLS;
        int tx  = S_GRID_H_MARG + col * (tile_w + S_GRID_H_MARG);
        int ty  = area_y0 + row * (tile_h + S_GRID_V_MARG);

        bool      focused = (slot == settings_category_cursor);
        pax_col_t bg      = focused ? COL_PAGER_ACCENT : COL_PAGER_TILE;
        pax_col_t fg      = focused ? COL_HEADER : COL_PAGER_TEXT;

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
        if (i < settings_category_icons_count && settings_category_icons[i]) {
            settings_category_icons[i](icon_cx, icon_cy, icon_sz, fg);
        }

        // Render label centered; support an embedded '\n' so labels that
        // overflow a tile (e.g. "Region &\nLocation") wrap onto a second
        // line instead of being clipped.
        const char* lbl     = s_categories[i].title;
        const char* nl      = strchr(lbl, '\n');
        int         ly_base = ty + tile_h * 3 / 4;
        if (nl) {
            char line1[40];
            int  n1 = nl - lbl;
            if (n1 >= (int)sizeof(line1)) n1 = sizeof(line1) - 1;
            memcpy(line1, lbl, n1);
            line1[n1]         = '\0';
            const char* line2 = nl + 1;
            pax_vec2f   sz1   = pax_text_size(FONT, TXT_BODY, line1);
            pax_vec2f   sz2   = pax_text_size(FONT, TXT_BODY, line2);
            int         ly1   = ly_base - (TXT_BODY / 2 + 2);
            int         ly2   = ly_base + (TXT_BODY / 2 + 2);
            pax_draw_text(&fb, fg, FONT, TXT_BODY, tx + (tile_w - (int)sz1.x) / 2, ly1, line1);
            pax_draw_text(&fb, fg, FONT, TXT_BODY, tx + (tile_w - (int)sz2.x) / 2, ly2, line2);
        } else {
            pax_vec2f lsz = pax_text_size(FONT, TXT_BODY, lbl);
            pax_draw_text(&fb, fg, FONT, TXT_BODY, tx + (tile_w - (int)lsz.x) / 2, ly_base, lbl);
        }
    }

    int fy = h - S_GRID_FOOTER;
    pax_simple_rect(&fb, COL_HEADER, 0, fy, w, S_GRID_FOOTER);
    pax_simple_rect(&fb, COL_PAGER_ACCENT, 0, fy, w, 1);
    {
        const char* hint    = "WSAD: nav   Enter: open   Tab: next view   ";
        int         hint_ty = fy + (S_GRID_FOOTER - TXT_SMALL) / 2;
        pax_draw_text(&fb, COL_HINT, FONT, TXT_SMALL, 10, hint_ty, hint);
        render_back_hint(10 + (int)pax_text_size(FONT, TXT_SMALL, hint).x, hint_ty, ": home", TXT_SMALL);
    }
}

// Optional inline section header drawn above a specific field's row. The
// Network drilldown groups its 12 rows into mesh / WiFi / HTTPS server, so
// the user can see at a glance which subsystem a field belongs to without
// repeating qualifiers on every label.
static const char* settings_section_above(field_t f) {
    switch (f) {
        case FIELD_WIFI_SSID:
            return "WiFi";
        case FIELD_HTTP_URL:
            return "HTTPS server";
        case FIELD_BLE_ENABLED:
            return "BLE companion";
        case FIELD_GPS_PROFILE:
            return "Tracking";
        case FIELD_MAP_PROFILE:
            return "Map";
        default:
            return NULL;
    }
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
        const char* mode_str = "[EDIT]";
        pax_vec2f   sz       = pax_text_size(FONT, TXT_SMALL, mode_str);
        pax_draw_text(&fb, COL_AMBER, FONT, TXT_SMALL, w - (int)sz.x - 110, (TAB_BAR_H - TXT_SMALL) / 2, mode_str);
    }

    row_t rows[FIELD_COUNT] = {0};
    // Build only the rows belonging to the active category. Label comes
    // from the registry, value from fmt_field(). Everything outside
    // [first_field, last_field] stays at the zero-init default and is
    // never touched by the renderer.
    for (int i = first_field; i <= last_field && i < FIELD_COUNT; i++) {
        rows[i].label = s_fields[i].label;
        fmt_field((field_t)i, rows[i].value, sizeof(rows[i].value));
    }

    const int row_h    = 44;
    const int sec_h    = 26;  // height of an inline section header above a row
    const int title_h  = 38;
    const int footer_h = 60;
    const int y0       = TAB_BAR_H + 6;

    // Category title carries a '\n' for the 2-line grid-tile rendering
    // ("Region &\nLocation"). The drilldown header is a single-line bar,
    // so flatten the newline to a space here -- otherwise pax_draw_text
    // wraps and "Location" ends up under "Region &".
    {
        const char* src = settings_category_title(settings_category_active);
        char        flat[40];
        size_t      j = 0;
        for (size_t i = 0; src[i] && j + 1 < sizeof(flat); i++) {
            flat[j++] = (src[i] == '\n') ? ' ' : src[i];
        }
        flat[j] = '\0';
        pax_draw_text(&fb, COL_AMBER, FONT, TXT_TITLE, 18, y0, flat);
    }
    pax_simple_rect(&fb, COL_AMBER, 18, y0 + TXT_TITLE + 4, w - 36, 1);

    int list_y0 = y0 + title_h;
    int list_h  = h - list_y0 - footer_h;

    if (selected < first_field) selected = first_field;
    if (selected > last_field) selected = last_field;

    // Pre-pass: compute the absolute y-offset of each field's row within the
    // virtual list, the position of the selected row, and the total content
    // height so scroll math survives variable-sized inline section headers
    // (currently only the Network drilldown opts in via settings_section_above).
    int field_y[FIELD_COUNT] = {0};  // y-top of each row, relative to list_y0+0 scroll
    int total_h              = 0;
    int sel_top = 0, sel_bot = 0;
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

    pax_clip(&fb, 0, list_y0, w, list_h);
    for (int f = first_field; f <= last_field; f++) {
        int y = list_y0 + field_y[f] - settings_scroll;

        // Render the section header (if any) just above this row.
        const char* hdr = settings_section_above((field_t)f);
        if (hdr) {
            int hy = y - sec_h;
            if (hy < list_y0 + list_h && hy + sec_h > list_y0) {
                pax_draw_text(&fb, COL_AMBER, FONT, TXT_SMALL, 18, hy + (sec_h - TXT_SMALL) / 2 - 1, hdr);
                pax_vec2f hsz    = pax_text_size(FONT, TXT_SMALL, hdr);
                int       line_x = 18 + (int)hsz.x + 10;
                int       line_y = hy + sec_h - 6;
                pax_simple_rect(&fb, COL_PANEL, line_x, line_y, w - line_x - 18, 1);
            }
        }

        if (y + row_h <= list_y0 || y >= list_y0 + list_h) continue;

        bool is_sel = (f == selected);
        if (is_sel) {
            pax_col_t bg  = edit_mode ? 0xFF3A2A1A : COL_PANEL;
            pax_col_t bar = edit_mode ? COL_AMBER : COL_ACCENT;
            pax_simple_rect(&fb, bg, 0, y, w, row_h - 1);
            pax_simple_rect(&fb, bar, 0, y, 5, row_h - 1);
        }
        pax_simple_rect(&fb, COL_PANEL, 12, y + row_h - 1, w - 24, 1);

        pax_col_t lbl_col = is_sel ? COL_WHITE : COL_GRAY;
        pax_draw_text(&fb, lbl_col, FONT, TXT_BODY, 18, y + text_y_off, rows[f].label);

        pax_col_t val_col;
        bool      regulatory_violation = false;
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

        char val_disp[80];
        bool is_text_field = (f == FIELD_OWNER || f == FIELD_ADV_NAME || f == FIELD_REGION_SCOPE ||
                              f == FIELD_GPS_LAT || f == FIELD_GPS_LON);
        if (is_sel && edit_mode && field_editing_text && is_text_field) {
            // Settings text fields are short; bound the slice so the shared
            // 128-byte field_edit_buf can't provoke a format-truncation warning.
            snprintf(val_disp, sizeof(val_disp), "%.76s_", field_edit_buf);
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

    const char* hint = NULL;
    // Red X back/cancel label, drawn after hint. Default = leave the drilled-in
    // category; the edit-mode branches switch it to cancel.
    const char* back = ": back to categories";
    char        hintbuf[128];
    pax_col_t   hint_col = COL_GRAY;
    if (edit_mode && field_editing_text) {
        hint = "Type to edit   Backspace: del   Enter: save   ";
        back = ": cancel";
    } else if (edit_mode) {
        hint = "Up/Down or W/S: adjust   Enter: save   ";
        back = ": cancel";
    } else if (!c6_available) {
        hint     = "NVS only — update radio via Launcher: Tools > Firmware update";
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
                snprintf(hintbuf, sizeof(hintbuf), "%s: %.3f MHz off-band — pick a frequency in an allowed sub-band.",
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
        // Two-line layout in the existing 60-px footer:
        //   top: yellow-square icon hint (matches home screen)
        //   bot: standard nav hint (kept so users don't lose discovery)
        hint              = NULL;
        int         top_y = fy + 4;
        int         bot_y = top_y + TXT_BODY + 4;
        const char* pre   = "Press ";
        const char* post  = " to blank / wake display";
        pax_draw_text(&fb, hint_col, FONT, TXT_BODY, 10, top_y, pre);
        pax_vec2f pre_sz  = pax_text_size(FONT, TXT_BODY, pre);
        int       icon_sz = TXT_BODY - 6;
        int       icon_x  = 10 + (int)pre_sz.x;
        int       icon_y  = top_y + (TXT_BODY - icon_sz) / 2;
        pax_simple_rect(&fb, COL_YELLOW, icon_x, icon_y, icon_sz, icon_sz);
        pax_draw_text(&fb, hint_col, FONT, TXT_BODY, icon_x + icon_sz + 4, top_y, post);
        const char* nav = "W/S: navigate   Enter: edit   R: reload   ";
        pax_draw_text(&fb, hint_col, FONT, TXT_SMALL, 10, bot_y, nav);
        render_back_hint(10 + (int)pax_text_size(FONT, TXT_SMALL, nav).x, bot_y, ": back", TXT_SMALL);
    } else {
        hint = "W/S: navigate   Enter: edit   R: reload   ";
    }
    if (hint) {
        pax_draw_text(&fb, hint_col, FONT, TXT_BODY, 10, fy + 6, hint);
        // Back hint on a second line so a long field hint can't overflow into it
        // or the "* unsaved" indicator on the right.
        if (back) {
            render_back_hint(10, fy + 6 + TXT_BODY + 4, back, TXT_SMALL);
        }
    }

    if (dirty) {
        const char* unsaved = "* unsaved";
        pax_vec2f   usz     = pax_text_size(FONT, TXT_BODY, unsaved);
        pax_draw_text(&fb, COL_AMBER, FONT, TXT_BODY, w - (int)usz.x - 10, fy + 6, unsaved);
    }
}

// ── fmt_field: central value-string formatter ──────────────────────────────
// One switch per field — replaces the 8 build_rows_* functions. Adding a new
// settings row means: one entry in s_fields[] (label/save) + one case here
// (value formatting). Labels live in the registry; only the dynamic value
// string is produced here.
static void fmt_field(field_t f, char* out, size_t cap) {
    switch (f) {
        // ── Identity ──
        case FIELD_RADIO_FW:
            snprintf(out, cap, "%s", radio_fw_version[0] ? radio_fw_version : "?");
            break;
        case FIELD_RADIO_FW_APP:
            snprintf(out, cap, "%s", radio_fw_app_version[0] ? radio_fw_app_version : TANMATSU_RADIO_FW_LABEL);
            break;
        case FIELD_OWNER:
            snprintf(out, cap, "%s", owner_name);
            break;
        case FIELD_ADV_NAME:
            snprintf(out, cap, "%s", lora_advert_name[0] ? lora_advert_name : "(use owner)");
            break;

        // ── Regulatory ──
        case FIELD_COUNTRY: {
            const regulatory_country_t* rc = region_get_country(country_code);
            if (!rc || rc->n_subbands == 0) {
                snprintf(out, cap, "(not set)");
            } else {
                const regulatory_subband_t* sb = region_match_subband(rc, (float)lora_cfg.frequency / 1000000.0f);
                snprintf(out, cap, sb ? "%s [%s]" : "%s [!off-band]", rc->display_name, sb ? sb->label : "");
            }
            break;
        }
        case FIELD_ANTENNA_GAIN:
            if (country_code[0] == '-' || country_code[0] == '\0') {
                snprintf(out, cap, "(set country)");
            } else {
                snprintf(out, cap, "%d dBi", (int)antenna_gain_dbi);
            }
            break;
        case FIELD_DUTY_CYCLE:
            if (dc_budget_ms == 0 || dc_budget_ms >= 3600000u) {
                snprintf(out, cap, "%lus used (no limit)", (unsigned long)(dc_used_ms / 1000u));
            } else {
                unsigned pct_x10 = (unsigned)(((uint64_t)dc_used_ms * 1000u) / dc_budget_ms);
                snprintf(out, cap, "%u.%u%% (%lus / %lus)%s", pct_x10 / 10u, pct_x10 % 10u,
                         (unsigned long)(dc_used_ms / 1000u), (unsigned long)(dc_budget_ms / 1000u),
                         dc_last_tx_blocked ? " BLOCKED" : "");
            }
            break;

        // ── Radio ──
        case FIELD_FREQ:
            snprintf(out, cap, "%.3f MHz", (double)lora_cfg.frequency / 1000000.0);
            break;
        case FIELD_SF:
            snprintf(out, cap, "SF%d", lora_cfg.spreading_factor);
            break;
        case FIELD_BW:
            snprintf(out, cap, "%d kHz", (int)lora_cfg.bandwidth);
            break;
        case FIELD_CR:
            snprintf(out, cap, "4/%d", lora_cfg.coding_rate);
            break;
        case FIELD_POWER:
            snprintf(out, cap, "%d dBm", (int)lora_cfg.power);
            break;
        case FIELD_SYNC:
            snprintf(out, cap, "0x%02X", (unsigned)lora_cfg.sync_word);
            break;
        case FIELD_PREAMBLE:
            snprintf(out, cap, "%d", (int)lora_cfg.preamble_length);
            break;
        case FIELD_PRESET: {
            int pidx = lora_preset_match();
            snprintf(out, cap, "%s", pidx >= 0 ? LORA_PRESETS[pidx].name : "(custom)");
            break;
        }
        case FIELD_SENSITIVITY:
            snprintf(out, cap, "%s", lora_cfg.rx_boost ? "High (+3dB)" : "Power save");
            break;

        // ── Advert ──
        // Format an interval in s/min/h depending on magnitude. "off" when
        // zero (the default for both flood + direct on a fresh badge).
        case FIELD_FLOOD_ADVERT_INT:
        case FIELD_DIRECT_ADVERT_INT: {
            unsigned secs = (f == FIELD_FLOOD_ADVERT_INT) ? flood_advert_interval_s : direct_advert_interval_s;
            if (secs == 0)
                snprintf(out, cap, "off");
            else if (secs < 60)
                snprintf(out, cap, "%us", secs);
            else if (secs < 3600)
                snprintf(out, cap, "%umin", secs / 60);
            else
                snprintf(out, cap, "%uh", secs / 3600);
            break;
        }
        case FIELD_SEND_FLOOD_NOW:
        case FIELD_SEND_DIRECT_NOW:
            snprintf(out, cap, "press OK");
            break;

        // ── Network ──
        case FIELD_ROLE:
            snprintf(out, cap, "%s", role_label(lora_role));
            break;
        case FIELD_PATH_HASH_SIZE: {
            static const char* hops[] = {"64 hops", "32 hops", "21 hops"};
            int                hi     = (path_hash_size >= 1 && path_hash_size <= 3) ? (path_hash_size - 1) : 0;
            snprintf(out, cap, "%u byte (%s)", path_hash_size, hops[hi]);
            break;
        }
        case FIELD_WIFI_SSID: {
            // Read-only: the SSID of the launcher slot we're set to use.
            // Falls back to "(no slots)" if the launcher has no networks
            // saved yet — the user should add one via the launcher first.
            int n = wifi_slots_count();
            if (n == 0) {
                snprintf(out, cap, "(no slots — use launcher)");
                break;
            }
            int pos = 0;
            for (int i = 0; i < n; i++) {
                if (wifi_slot_idx_at(i) == wifi_slot) {
                    pos = i;
                    break;
                }
            }
            snprintf(out, cap, "%s", wifi_slot_ssid_at(pos));
            break;
        }
        case FIELD_WIFI_NETWORK: {
            int n = wifi_slots_count();
            if (n == 0) {
                snprintf(out, cap, "(no slots)");
                break;
            }
            int pos = 0;
            for (int i = 0; i < n; i++) {
                if (wifi_slot_idx_at(i) == wifi_slot) {
                    pos = i;
                    break;
                }
            }
            snprintf(out, cap, "[%u] %s", (unsigned)wifi_slot_idx_at(pos), wifi_slot_ssid_at(pos));
            break;
        }
        case FIELD_WIFI_ENABLED:
            snprintf(out, cap, "%s", wifi_enabled ? "On" : "Off");
            break;
        case FIELD_WIFI_STATUS:
            if (wifi_connection_is_connected()) {
                esp_netif_ip_info_t* ip = wifi_get_ip_info();
                // The SSID we're actually on can differ from wifi_ssid when
                // the launcher slot is still associated. Surfacing it makes
                // the "edit didn't stick" case obvious instead of silent.
                wifi_ap_record_t     ap = {0};
                const char* cur = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK && ap.ssid[0]) ? (const char*)ap.ssid : "?";
                if (ip && ip->ip.addr)
                    snprintf(out, cap, "%s @ " IPSTR, cur, IP2STR(&ip->ip));
                else
                    snprintf(out, cap, "%s (no IP yet)", cur);
            } else {
                snprintf(out, cap, "Disconnected");
            }
            break;
        case FIELD_HTTP_URL:
            if (wifi_connection_is_connected()) {
                esp_netif_ip_info_t* ip = wifi_get_ip_info();
                if (ip && ip->ip.addr)
                    snprintf(out, cap, "https://" IPSTR ":8443/ping", IP2STR(&ip->ip));
                else
                    snprintf(out, cap, "(no IP yet)");
            } else {
                snprintf(out, cap, "(WiFi disconnected)");
            }
            break;
        case FIELD_HTTP_API_KEY:
            // First 8 + last 4 chars only -- enough to visually confirm a match
            // without exposing the full secret on every glance. Full key still
            // grabbable from serial log.
            if (http_api_key[0])
                snprintf(out, cap, "%.8s...%.4s", http_api_key, http_api_key + 60);
            else
                snprintf(out, cap, "(not set)");
            break;
        case FIELD_HTTP_KEY_REGEN:
            snprintf(out, cap, "press OK to roll");
            break;
        case FIELD_HTTPS_CERT_FP: {
            const char* cert_pem = http_server_cert_pem();
            if (cert_pem) {
                char fp[65] = {0};
                if (cert_gen_fingerprint_hex(cert_pem, fp, sizeof(fp)) == ESP_OK) {
                    snprintf(out, cap, "%.8s...%.4s", fp, fp + 60);
                } else {
                    snprintf(out, cap, "(parse error)");
                }
            } else {
                snprintf(out, cap, "(not yet generated)");
            }
            break;
        }
        case FIELD_HTTPS_CERT_REGEN:
            snprintf(out, cap, "press OK (~2 s)");
            break;
        case FIELD_HTTP_QR:
            snprintf(out, cap, "press OK to show QR");
            break;

        // ── Region & Location ──
        case FIELD_REGION_SCOPE:
            snprintf(out, cap, "%s", region_scope[0] ? region_scope : "(not set)");
            break;
        case FIELD_GPS_LAT:
            if (gps_position_valid)
                snprintf(out, cap, "%.6f", (double)gps_lat_e6 / 1e6);
            else
                snprintf(out, cap, "(not set)");
            break;
        case FIELD_GPS_LON:
            if (gps_position_valid)
                snprintf(out, cap, "%.6f", (double)gps_lon_e6 / 1e6);
            else
                snprintf(out, cap, "(not set)");
            break;
        case FIELD_GPS_SOURCE: {
            const char* src_str = "(none)";
            switch (gps_last_source) {
                case GPS_SRC_MANUAL:
                    src_str = "Manual";
                    break;
                case GPS_SRC_PA1010D:
                    src_str = "PA1010D GPS";
                    break;
                case GPS_SRC_CDC:
                    src_str = "USB-CDC push";
                    break;
                case GPS_SRC_BLE:
                    src_str = "BLE companion";
                    break;
                case GPS_SRC_HTTP:
                    src_str = "HTTPS /ping";
                    break;
                case GPS_SRC_NONE:
                    break;
            }
            snprintf(out, cap, "%s", src_str);
            break;
        }
        case FIELD_GPS_AUTOFILL: {
            // After the first scan this session show a compact summary of
            // the latest fix so the result survives the toast fade-out.
            gps_status_t last;
            if (gps_last_status(&last)) {
                if (last.fix_valid) {
                    snprintf(out, cap, "Last: %d sats, HDOP %.1f", last.fix_used_sats, (double)last.hdop);
                } else {
                    int sats_view = last.gps_sats_view + last.glo_sats_view;
                    snprintf(out, cap, "No fix - %d sats seen", sats_view);
                }
            } else {
                snprintf(out, cap, "press OK to scan");
            }
            break;
        }
        case FIELD_GPS_PROFILE:
            snprintf(out, cap, "%s", gps_profile_label(gps_profile));
            break;
        case FIELD_GPS_INTERVAL_S:
            if (gps_custom_interval_s == 0)
                snprintf(out, cap, "Auto");
            else
                snprintf(out, cap, "%us", (unsigned)gps_custom_interval_s);
            break;
        case FIELD_GPS_DISTANCE_M:
            if (gps_custom_distance_m == 0)
                snprintf(out, cap, "Auto");
            else
                snprintf(out, cap, "%um", (unsigned)gps_custom_distance_m);
            break;
        case FIELD_MAP_PROFILE:
            snprintf(out, cap, "%s", map_profile_label(map_profile));
            break;
        case FIELD_BLE_ENABLED:
            snprintf(out, cap, "%s", ble_enabled ? "On" : "Off");
            break;

        // ── Brightness ──
        case FIELD_DISPLAY_BL:
            snprintf(out, cap, "%u%%", (unsigned)display_brightness);
            break;
        case FIELD_KB_BL:
            snprintf(out, cap, "%u%%", (unsigned)keyboard_brightness);
            break;
        case FIELD_LED_BR:
            snprintf(out, cap, "%u%%", (unsigned)led_brightness);
            break;
        case FIELD_BLANK_AFTER:
            if (display_blank_after_s == 0)
                snprintf(out, cap, "Off");
            else if (display_blank_after_s < 60)
                snprintf(out, cap, "%us", (unsigned)display_blank_after_s);
            else
                snprintf(out, cap, "%umin", (unsigned)(display_blank_after_s / 60));
            break;

        // ── Sounds ──
        case FIELD_SOUND_VOLUME:
            snprintf(out, cap, "%u%%", (unsigned)sound_volume_pct);
            break;
        case FIELD_SOUND_DM:
        case FIELD_SOUND_CHANNEL:
        case FIELD_SOUND_ERROR:
        case FIELD_SOUND_BOOT: {
            uint8_t slot = (f == FIELD_SOUND_DM)        ? sound_dm_slot
                           : (f == FIELD_SOUND_CHANNEL) ? sound_channel_slot
                           : (f == FIELD_SOUND_ERROR)   ? sound_error_slot
                                                        : sound_boot_slot;
            if (slot == 0) {
                snprintf(out, cap, "Off");
            } else if (slot > sounds_count()) {
                // Stored slot references a WAV that's no longer on the SD.
                snprintf(out, cap, "(missing #%u)", (unsigned)slot);
            } else {
                snprintf(out, cap, "%s", sounds_slot_name(slot));
            }
            break;
        }
        case FIELD_SOUND_TEST_DM:
        case FIELD_SOUND_TEST_CHANNEL:
        case FIELD_SOUND_TEST_ERROR:
        case FIELD_SOUND_TEST_BOOT:
            snprintf(out, cap, "press OK");
            break;

        case FIELD_COUNT:
            // Not a real field; guards the switch's exhaustiveness warning.
            break;
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
