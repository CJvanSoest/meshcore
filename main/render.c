// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "render.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp/display.h"
#include "bsp/power.h"
#include "bsp/tanmatsu.h"
#include "esp_log.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"

#include "app_config.h"
#include "channels.h"
#include "chat.h"
#include "contacts.h"
#include "emoji.h"
#include "identity.h"
#include "nodes.h"
#include "qrcodegen.h"
#include "radio.h"
#include "region_limits.h"
#include "settings_nvs.h"
#include "ui_state.h"

static const char *TAG = "render";

// c6_available lives in main.c (set during boot once the C6 responds).
extern bool c6_available;

size_t    display_h_res = 0;
size_t    display_v_res = 0;
pax_buf_t fb            = {0};

void blit(void) {
    esp_err_t res = bsp_display_blit(0, 0, display_h_res, display_v_res, pax_buf_get_pixels(&fb));
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "blit failed: %d", res);
    }
}

static void render_tab_bar(void) {
    int w = (int)pax_buf_get_width(&fb);
    static const char *tab_labels[VIEW_COUNT] = {"Settings", "Nodes", "DM", "Channel"};
    int tab_w = (w - 200) / VIEW_COUNT;  // reserve 200px right for status indicators

    pax_simple_rect(&fb, COL_HEADER, 0, 0, w, TAB_BAR_H);

    int label_y = (TAB_BAR_H - TXT_TAB) / 2;
    for (int i = 0; i < VIEW_COUNT; i++) {
        bool active = (i == (int)current_view);
        if (active) {
            pax_simple_rect(&fb, COL_ACCENT, i * tab_w, 0, tab_w, TAB_BAR_H);
        }
        pax_col_t col = active ? COL_HEADER : COL_GRAY;
        pax_vec2f sz  = pax_text_size(FONT, TXT_TAB, tab_labels[i]);

        // Unread badge: red pill with count, drawn right of the label.
        // Tab total = sum over all conversations; clears as each is opened.
        int unread = (i == VIEW_CHAT) ? contact_unread_total()
                   : (i == VIEW_CHANNEL) ? channel_unread_total() : 0;
        int badge_w = 0;
        char badge_txt[8] = {0};
        if (unread > 0) {
            snprintf(badge_txt, sizeof(badge_txt), "%d", unread > 99 ? 99 : unread);
            pax_vec2f bsz = pax_text_size(FONT, TXT_SMALL, badge_txt);
            badge_w = (int)bsz.x + 12;  // 6px padding each side
        }

        int total_w = (int)sz.x + (badge_w ? badge_w + 6 : 0);
        int tx      = i * tab_w + (tab_w - total_w) / 2;
        pax_draw_text(&fb, col, FONT, TXT_TAB, tx, label_y, tab_labels[i]);

        if (badge_w) {
            int bx = tx + (int)sz.x + 6;
            int by = (TAB_BAR_H - TXT_SMALL) / 2 - 2;
            int bh = TXT_SMALL + 4;
            pax_simple_rect(&fb, COL_RED, bx, by, badge_w, bh);
            pax_vec2f tsz = pax_text_size(FONT, TXT_SMALL, badge_txt);
            int label_tx = bx + (badge_w - (int)tsz.x) / 2;
            pax_draw_text(&fb, COL_HEADER, FONT, TXT_SMALL, label_tx, by + 2, badge_txt);
        }
    }
    pax_simple_rect(&fb, COL_PANEL, 0, TAB_BAR_H - 1, w, 1);

    char status_right[32] = {0};
    int  status_x         = w - 10;
    int  status_y         = (TAB_BAR_H - TXT_BODY) / 2;

    bsp_power_battery_information_t bat = {0};
    if (bsp_power_get_battery_information(&bat) == ESP_OK && bat.battery_available) {
        int pct = (int)bat.remaining_percentage;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        const char *chr = bat.battery_charging ? "+" : "";
        snprintf(status_right, sizeof(status_right), "%d%%%s", pct, chr);
        pax_col_t bat_col = pct <= 20 ? COL_RED : (pct <= 50 ? COL_AMBER : COL_GREEN);
        pax_vec2f sz = pax_text_size(FONT, TXT_BODY, status_right);
        status_x -= (int)sz.x;
        pax_draw_text(&fb, bat_col, FONT, TXT_BODY, status_x, status_y, status_right);
        status_x -= 14;
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_BODY, status_x, status_y, "|");
        status_x -= 14;
    }

    if (lora_rx_ok) {
        int cnt = 0;
        if (xSemaphoreTake(rx_mutex, 0) == pdTRUE) {
            cnt = rx_count;
            xSemaphoreGive(rx_mutex);
        }
        char rxbuf[12];
        snprintf(rxbuf, sizeof(rxbuf), "RX:%d", cnt);
        pax_vec2f sz = pax_text_size(FONT, TXT_BODY, rxbuf);
        status_x -= (int)sz.x;
        pax_draw_text(&fb, COL_GREEN, FONT, TXT_BODY, status_x, status_y, rxbuf);
    }
}

static void render_settings(void) {
    int w = (int)pax_buf_get_width(&fb);
    int h = (int)pax_buf_get_height(&fb);

    pax_background(&fb, COL_BLACK);
    render_tab_bar();

    if (edit_mode) {
        const char *mode_str = "[EDIT]";
        pax_vec2f sz = pax_text_size(FONT, TXT_SMALL, mode_str);
        pax_draw_text(&fb, COL_AMBER, FONT, TXT_SMALL, w - (int)sz.x - 110,
                      (TAB_BAR_H - TXT_SMALL) / 2, mode_str);
    }

    typedef struct { const char *label; char value[64]; } row_t;
    row_t rows[FIELD_COUNT];

    // lora_get_status returns the SX126x silicon version-register string,
    // which reads "sx1261 ..." even on an SX1262 (shared silicon). Shown as
    // "Radio ID"; the resolved part type is reported separately via chip_type.
    rows[FIELD_RADIO_FW].label = "Radio ID";
    snprintf(rows[FIELD_RADIO_FW].value, sizeof(rows[FIELD_RADIO_FW].value), "%s",
             radio_fw_version[0] ? radio_fw_version : "?");

    // App firmware version from C6 (via GET_FW_VERSION). Falls back to the
    // hand-maintained TANMATSU_RADIO_FW_LABEL on C6 firmware lacking the cmd.
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
        // x10 fixed-point: pct_x10 = used * 1000 / budget. 360s budget,
        // 36 ms airtime → 36*1000/360000 = 0.1%.
        unsigned pct_x10 = (unsigned)(((uint64_t)dc_used_ms * 1000u) / dc_budget_ms);
        snprintf(rows[FIELD_DUTY_CYCLE].value, sizeof(rows[FIELD_DUTY_CYCLE].value),
                 "%u.%u%% (%lus / %lus)%s",
                 pct_x10 / 10u, pct_x10 % 10u,
                 (unsigned long)(dc_used_ms / 1000u),
                 (unsigned long)(dc_budget_ms / 1000u),
                 dc_last_tx_blocked ? " BLOCKED" : "");
    }

    // ── Layout + section headings ────────────────────────────────────────────
    // Enum order == display order; a heading is inserted before its first field
    // and scrolls with the list. Pixel-based scroll so headings can be shorter
    // than field rows.
    static const struct { field_t first; const char *title; } sections[] = {
        { FIELD_RADIO_FW,     "DEVICE & IDENTITY" },
        { FIELD_COUNTRY,      "REGULATORY" },
        { FIELD_FREQ,         "RADIO" },
        { FIELD_ADVERT_INT,   "NETWORK & BEHAVIOR" },
        { FIELD_REGION_SCOPE, "REGION & LOCATION" },
    };
    const int n_sections = (int)(sizeof(sections) / sizeof(sections[0]));

    const int row_h    = 44;
    const int head_h   = 32;
    const int footer_h = 60;
    const int y0       = TAB_BAR_H + 6;
    const int list_h   = h - y0 - footer_h;

    typedef struct { bool heading; const char *title; int field; int y; int hgt; } disp_t;
    disp_t disp[FIELD_COUNT + 8];
    int n_disp = 0, content_h = 0, sel_disp = 0;
    for (int f = 0; f < FIELD_COUNT; f++) {
        for (int s = 0; s < n_sections; s++) {
            if ((int)sections[s].first == f) {
                disp[n_disp] = (disp_t){ true, sections[s].title, -1, content_h, head_h };
                content_h += head_h;
                n_disp++;
                break;
            }
        }
        disp[n_disp] = (disp_t){ false, NULL, f, content_h, row_h };
        if (f == selected) sel_disp = n_disp;
        content_h += row_h;
        n_disp++;
    }

    // Keep the selected field visible; reveal its heading too when it leads a section.
    int reveal_top = disp[sel_disp].y;
    if (sel_disp > 0 && disp[sel_disp - 1].heading) reveal_top = disp[sel_disp - 1].y;
    int reveal_bot = disp[sel_disp].y + disp[sel_disp].hgt;
    if (reveal_top < settings_scroll)          settings_scroll = reveal_top;
    if (reveal_bot > settings_scroll + list_h) settings_scroll = reveal_bot - list_h;
    int max_scroll = content_h - list_h;
    if (max_scroll < 0)               max_scroll = 0;
    if (settings_scroll > max_scroll)  settings_scroll = max_scroll;
    if (settings_scroll < 0)           settings_scroll = 0;

    int text_y_off = (row_h - TXT_BODY) / 2;

    pax_clip(&fb, 0, y0, w, list_h);
    int first_vis = -1, last_vis = -1;
    for (int d = 0; d < n_disp; d++) {
        int y = y0 + disp[d].y - settings_scroll;
        if (y + disp[d].hgt <= y0 || y >= y0 + list_h) continue;  // fully off-screen

        if (disp[d].heading) {
            pax_draw_text(&fb, COL_AMBER, FONT, TXT_SMALL, 18,
                          y + head_h - TXT_SMALL - 5, disp[d].title);
            pax_simple_rect(&fb, COL_AMBER, 18, y + head_h - 3, w - 36, 1);
            continue;
        }

        int  i      = disp[d].field;
        bool is_sel = (i == selected);
        if (first_vis < 0) first_vis = i;
        last_vis = i;

        if (is_sel) {
            pax_col_t bg  = edit_mode ? 0xFF3A2A1A : COL_PANEL;
            pax_col_t bar = edit_mode ? COL_AMBER  : COL_ACCENT;
            pax_simple_rect(&fb, bg,  0, y, w, row_h - 1);
            pax_simple_rect(&fb, bar, 0, y, 5, row_h - 1);
        }

        pax_simple_rect(&fb, COL_PANEL, 12, y + row_h - 1, w - 24, 1);

        pax_col_t lbl_col = is_sel ? COL_WHITE : COL_GRAY;
        pax_draw_text(&fb, lbl_col, FONT, TXT_BODY, 18, y + text_y_off, rows[i].label);

        pax_col_t val_col;
        bool regulatory_violation = false;
        if (i == FIELD_FREQ || i == FIELD_POWER || i == FIELD_COUNTRY) {
            const regulatory_country_t *rc = region_get_country(country_code);
            if (rc && rc->n_subbands > 0) {
                const regulatory_subband_t *sb = region_match_subband(
                    rc, (float)lora_cfg.frequency / 1000000.0f);
                if (!sb) {
                    regulatory_violation = true;  // freq outside any sub-band
                } else if (i == FIELD_POWER) {
                    int8_t eff = region_effective_power_dbm(rc, (int8_t)lora_cfg.power, antenna_gain_dbi);
                    if (eff > sb->max_power_dbm) regulatory_violation = true;
                }
            }
        }
        if (i >= FIELD_FREQ && !c6_available) {
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
        bool is_text_field = (i == FIELD_OWNER || i == FIELD_ADV_NAME ||
                              i == FIELD_REGION_SCOPE ||
                              i == FIELD_GPS_LAT || i == FIELD_GPS_LON);
        if (is_sel && edit_mode && field_editing_text && is_text_field) {
            snprintf(val_disp, sizeof(val_disp), "%s_", field_edit_buf);
        } else if (is_sel && edit_mode && !is_text_field) {
            snprintf(val_disp, sizeof(val_disp), "< %s >", rows[i].value);
        } else {
            snprintf(val_disp, sizeof(val_disp), "%s", rows[i].value);
        }
        pax_vec2f vsz = pax_text_size(FONT, TXT_BODY, val_disp);
        pax_draw_text(&fb, val_col, FONT, TXT_BODY, w - (int)vsz.x - 18, y + text_y_off, val_disp);
    }
    pax_noclip(&fb);

    if (content_h > list_h && first_vis >= 0) {
        char sc[40];
        snprintf(sc, sizeof(sc), "%d-%d/%d", first_vis + 1, last_vis + 1, FIELD_COUNT);
        pax_vec2f sz = pax_text_size(FONT, TXT_BODY, sc);
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_BODY, w - (int)sz.x - 10, h - footer_h - TXT_BODY - 2, sc);
    }

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
    } else {
        hint = "W/S: navigate   Enter: edit   R: reload   Tab: next";
    }
    pax_draw_text(&fb, hint_col, FONT, TXT_BODY, 10, fy + 6, hint);

    if (dirty) {
        const char *unsaved = "* unsaved";
        pax_vec2f usz = pax_text_size(FONT, TXT_BODY, unsaved);
        pax_draw_text(&fb, COL_AMBER, FONT, TXT_BODY, w - (int)usz.x - 10, fy + 6, unsaved);
    }

    int row2_y = fy + 6 + TXT_BODY + 6;
    {
        time_t    now = time(NULL);
        struct tm t;
        localtime_r(&now, &t);
        char ts[48];
        pax_col_t ts_col;
        if (identity_sntp_synced()) {
            snprintf(ts, sizeof(ts), "SNTP %02d:%02d:%02d  %02d-%02d-%04d",
                     t.tm_hour, t.tm_min, t.tm_sec,
                     t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
            ts_col = COL_GREEN;
        } else if (time_from_nvs) {
            snprintf(ts, sizeof(ts), "~%02d:%02d %02d-%02d (NVS, approx)",
                     t.tm_hour, t.tm_min, t.tm_mday, t.tm_mon + 1);
            ts_col = COL_AMBER;
        } else {
            snprintf(ts, sizeof(ts), "no time sync — timestamps incorrect");
            ts_col = COL_RED;
        }
        pax_draw_text(&fb, ts_col, FONT, TXT_BODY, 10, row2_y, ts);
    }
    {
        char rf[64];
        int  snr_dB = (int)last_rx_snr_db_x4 / 4;
        if (last_rx_stats_valid && noise_floor_valid) {
            snprintf(rf, sizeof(rf), "RX:%d SNR:%+d N:%d",
                     (int)last_rx_rssi_dbm, snr_dB, (int)noise_floor_dbm);
        } else if (last_rx_stats_valid) {
            snprintf(rf, sizeof(rf), "RX:%d SNR:%+d", (int)last_rx_rssi_dbm, snr_dB);
        } else if (noise_floor_valid) {
            snprintf(rf, sizeof(rf), "noise:%d", (int)noise_floor_dbm);
        } else {
            rf[0] = '\0';
        }
        if (rf[0]) {
            pax_vec2f rsz = pax_text_size(FONT, TXT_BODY, rf);
            pax_draw_text(&fb, COL_GRAY, FONT, TXT_BODY, w - (int)rsz.x - 10, row2_y, rf);
        }
    }
}

static void render_qr_overlay(void) {
    int w = (int)pax_buf_get_width(&fb);
    int h = (int)pax_buf_get_height(&fb);

    char hex_key[65];
    for (int i = 0; i < 32; i++) snprintf(&hex_key[i * 2], 3, "%02x", node_pub_key[i]);
    hex_key[64] = '\0';

    // Use same name as send_advert: lora_advert_name overrides owner_name when set.
    const char *adv_src = lora_advert_name[0] ? lora_advert_name :
                          ((owner_name[0] && owner_name[0] != '(') ? owner_name : "");

    char encoded_name[64];
    int ei = 0;
    for (int i = 0; adv_src[i] && ei < 62; i++) {
        char c = adv_src[i];
        if (c == ' ') { encoded_name[ei++] = '+'; }
        else          { encoded_name[ei++] = c; }
    }
    encoded_name[ei] = '\0';

    char url[200];
    snprintf(url, sizeof(url),
             "meshcore://contact/add?name=%s&public_key=%s&type=1",
             encoded_name, hex_key);

    // Static buffers to avoid stack overflow (~3900 bytes each).
    static uint8_t qr_data[qrcodegen_BUFFER_LEN_MAX];
    static uint8_t tmp_buf[qrcodegen_BUFFER_LEN_MAX];
    bool ok = qrcodegen_encodeText(url, tmp_buf, qr_data,
                                   qrcodegen_Ecc_MEDIUM,
                                   qrcodegen_VERSION_MIN, 10,
                                   qrcodegen_Mask_AUTO, true);

    pax_background(&fb, COL_BG);

    if (!ok) {
        pax_draw_text(&fb, COL_AMBER, FONT, TXT_BODY, 20, h / 2, "QR encode failed");
        return;
    }

    int qr_size = qrcodegen_getSize(qr_data);
    int max_px  = (h * 6) / 10;
    int cell_px = max_px / qr_size;
    if (cell_px < 2) cell_px = 2;
    int qr_px   = cell_px * qr_size;
    int qr_x    = (w - qr_px) / 2;
    int qr_y    = (h - qr_px) / 2;

    int margin = cell_px * 2;
    pax_simple_rect(&fb, 0xFFFFFFFF,
                    qr_x - margin, qr_y - margin,
                    qr_px + margin * 2, qr_px + margin * 2);

    for (int row = 0; row < qr_size; row++) {
        for (int col = 0; col < qr_size; col++) {
            if (qrcodegen_getModule(qr_data, col, row)) {
                pax_simple_rect(&fb, 0xFF000000,
                                qr_x + col * cell_px,
                                qr_y + row * cell_px,
                                cell_px, cell_px);
            }
        }
    }

    const char *label = "Scan to add contact";
    pax_vec2f lsz = pax_text_size(FONT, TXT_TITLE, label);
    pax_draw_text(&fb, COL_AMBER, FONT, TXT_TITLE,
                  (w - (int)lsz.x) / 2, qr_y - margin - TXT_TITLE - 6, label);

    char name_label[80];
    snprintf(name_label, sizeof(name_label), "%s  [press any key to close]",
             adv_src[0] ? adv_src : "(no name)");
    pax_vec2f nsz = pax_text_size(FONT, TXT_SMALL, name_label);
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL,
                  (w - (int)nsz.x) / 2, qr_y + qr_px + margin + 6, name_label);
}

#define NODES_ROW_H    44
#define NODES_Y0       (TAB_BAR_H + 4)
#define NODES_HEADER_H 26

static void render_nodes(void) {
    int w = (int)pax_buf_get_width(&fb);
    int h = (int)pax_buf_get_height(&fb);

    pax_background(&fb, COL_BG);
    render_tab_bar();

    int hdr_y = NODES_Y0;
    pax_simple_rect(&fb, COL_HEADER, 0, hdr_y, w, NODES_HEADER_H);
    pax_simple_rect(&fb, COL_PANEL,  0, hdr_y + NODES_HEADER_H - 1, w, 1);

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
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 8,            hdr_text_y, "Role");
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 96,           hdr_text_y, "Name");
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, rssi_hdr_x,   hdr_text_y, "RSSI");
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, snr_hdr_x,    hdr_text_y, "SNR");
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, pkts_hdr_x,   hdr_text_y, "#Pkt");
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, dist_hdr_x,   hdr_text_y, "Dist");
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, age_hdr_x,    hdr_text_y, "Seen");

    int list_y0   = NODES_Y0 + NODES_HEADER_H;
    int footer_h  = 60;
    int list_h    = h - footer_h - list_y0;
    int rows_vis  = list_h / NODES_ROW_H;
    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    int row_text_y = (NODES_ROW_H - TXT_BODY) / 2;

    if (!lora_rx_ok) {
        pax_draw_text(&fb, COL_AMBER, FONT, TXT_BODY, 12, list_y0 + 14, "LoRa radio not available");
        pax_draw_text(&fb, COL_GRAY,  FONT, TXT_BODY, 12, list_y0 + 14 + TXT_BODY + 8,
                      "Update via Launcher: Tools > Firmware update");
    } else if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (node_count == 0 && contact_count == 0) {
            pax_draw_text(&fb, COL_GRAY, FONT, TXT_BODY, 12, list_y0 + 14, "Listening... no nodes heard yet.");
        } else {
            display_row_t rows_dl[MAX_CONTACTS + MAX_NODES];
            int idx_count = build_node_display(rows_dl, MAX_CONTACTS + MAX_NODES);

            int max_scroll = idx_count - rows_vis;
            if (max_scroll < 0) max_scroll = 0;
            if (node_scroll > max_scroll) node_scroll = max_scroll;
            if (node_scroll < 0)         node_scroll = 0;

            if (node_cursor >= idx_count) node_cursor = idx_count > 0 ? idx_count - 1 : 0;
            if (node_cursor < 0)          node_cursor = 0;
            if (node_cursor < node_scroll)              node_scroll = node_cursor;
            if (node_cursor >= node_scroll + rows_vis)  node_scroll = node_cursor - rows_vis + 1;

            for (int row = 0; row < rows_vis; row++) {
                int list_idx = row + node_scroll;
                if (list_idx >= idx_count) break;
                display_row_t *d = &rows_dl[list_idx];
                node_entry_t  *n = (d->node_idx >= 0) ? &node_list[d->node_idx] : NULL;

                int y = list_y0 + row * NODES_ROW_H;
                bool is_cursor = (list_idx == node_cursor);

                if (is_cursor) {
                    pax_simple_rect(&fb, COL_PANEL, 0, y, w, NODES_ROW_H);
                    pax_simple_rect(&fb, COL_ACCENT, 0, y, 5, NODES_ROW_H);
                }

                int age_x  = w - age_col_w  - 6;
                int dist_x = age_x - dist_col_w;
                int pkts_x = dist_x - pkts_col_w;
                int snr_x  = pkts_x - snr_col_w;
                int rssi_x = snr_x - rssi_col_w;

                char age_buf[20];
                if (n) {
                    uint32_t age_s = (now_ms - n->last_seen_ms) / 1000;
                    if (age_s < 60)        snprintf(age_buf, sizeof(age_buf), "%lus", (unsigned long)age_s);
                    else if (age_s < 3600) snprintf(age_buf, sizeof(age_buf), "%lum", (unsigned long)(age_s / 60));
                    else                   snprintf(age_buf, sizeof(age_buf), "%luh", (unsigned long)(age_s / 3600));
                } else {
                    snprintf(age_buf, sizeof(age_buf), "--");
                }
                pax_draw_text(&fb, COL_GRAY, FONT, TXT_BODY, age_x, y + row_text_y, age_buf);

                char dist_buf[12];
                if (n && n->position_valid && gps_position_valid) {
                    // Equirectangular approximation — accurate enough for
                    // mesh-LoRa ranges (< few hundred km) and avoids the cost
                    // of haversine's sin/asin/sqrt chain per node per frame.
                    // R_earth = 6371 km. e6 -> radians: deg * 1e-6 * pi/180.
                    const double k = 1e-6 * (M_PI / 180.0);
                    double lat1 = (double)gps_lat_e6 * k;
                    double lon1 = (double)gps_lon_e6 * k;
                    double lat2 = (double)n->lat     * k;
                    double lon2 = (double)n->lon     * k;
                    double x = (lon2 - lon1) * cos((lat1 + lat2) * 0.5);
                    double y_ = (lat2 - lat1);
                    double d_km = 6371.0 * sqrt(x * x + y_ * y_);
                    if (d_km < 1.0)
                        snprintf(dist_buf, sizeof(dist_buf), "%dm", (int)(d_km * 1000.0));
                    else if (d_km < 10.0)
                        snprintf(dist_buf, sizeof(dist_buf), "%.1fkm", d_km);
                    else
                        snprintf(dist_buf, sizeof(dist_buf), "%dkm", (int)d_km);
                } else {
                    snprintf(dist_buf, sizeof(dist_buf), "--");
                }
                pax_draw_text(&fb, COL_GRAY, FONT, TXT_BODY, dist_x, y + row_text_y, dist_buf);

                char pkts_buf[8];
                if (n) snprintf(pkts_buf, sizeof(pkts_buf), "#%d", n->packet_count);
                else   snprintf(pkts_buf, sizeof(pkts_buf), "--");
                pax_draw_text(&fb, COL_GRAY, FONT, TXT_BODY, pkts_x, y + row_text_y, pkts_buf);

                char rssi_buf[8], snr_buf[8];
                pax_col_t rssi_col, snr_col;
                if (n && n->stats_valid) {
                    int rssi_dbm = (int)n->last_rssi_dbm;
                    int snr_dB   = (int)n->last_snr_db_x4 / 4;
                    snprintf(rssi_buf, sizeof(rssi_buf), "%d", rssi_dbm);
                    snprintf(snr_buf,  sizeof(snr_buf),  "%+d", snr_dB);
                    rssi_col = (rssi_dbm >= -80)  ? COL_GREEN :
                               (rssi_dbm >= -105) ? COL_AMBER : COL_RED;
                    snr_col  = (snr_dB  >=  0)    ? COL_GREEN :
                               (snr_dB  >= -10)   ? COL_AMBER : COL_RED;
                } else {
                    snprintf(rssi_buf, sizeof(rssi_buf), "--");
                    snprintf(snr_buf,  sizeof(snr_buf),  "--");
                    rssi_col = COL_GRAY;
                    snr_col  = COL_GRAY;
                }
                pax_draw_text(&fb, rssi_col, FONT, TXT_BODY, rssi_x, y + row_text_y, rssi_buf);
                pax_draw_text(&fb, snr_col,  FONT, TXT_BODY, snr_x,  y + row_text_y, snr_buf);

                meshcore_device_role_t role = n ? n->role : (meshcore_device_role_t)contacts[d->contact_idx].role;
                const char *src_name = n ? n->name : contacts[d->contact_idx].alias;

                const char *rl = role_label(role);
                pax_col_t role_col = (role == MESHCORE_DEVICE_ROLE_REPEATER)    ? COL_BLUE :
                                     (role == MESHCORE_DEVICE_ROLE_ROOM_SERVER) ? 0xFFBB9AF7 :
                                     (role == MESHCORE_DEVICE_ROLE_SENSOR)      ? COL_AMBER :
                                                                                  COL_GREEN;
                pax_draw_text(&fb, role_col, FONT, TXT_BODY, 8, y + row_text_y, rl);

                int name_x = 96;
                if (d->is_contact) {
                    pax_draw_text(&fb, COL_AMBER, FONT, TXT_BODY, 78, y + row_text_y, "*");
                }

                char name_trunc[25];
                int  max_name_w = rssi_x - name_x - 6;
                int  max_chars  = max_name_w / 11;
                if (max_chars > 24) max_chars = 24;
                if (max_chars < 1)  max_chars = 1;
                snprintf(name_trunc, sizeof(name_trunc), "%.*s", max_chars, src_name);
                pax_col_t name_col = is_cursor ? COL_WHITE :
                                     (n == NULL ? COL_GRAY : COL_WHITE);
                pax_draw_text(&fb, name_col, FONT, TXT_BODY, name_x, y + row_text_y, name_trunc);

                pax_simple_rect(&fb, COL_PANEL, 12, y + NODES_ROW_H - 1, w - 24, 1);
            }

            if (idx_count > rows_vis) {
                char sc[24];
                snprintf(sc, sizeof(sc), "%d/%d", node_scroll + 1, idx_count);
                pax_vec2f sz = pax_text_size(FONT, TXT_SMALL, sc);
                pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, w - (int)sz.x - 10, h - footer_h - TXT_SMALL - 2, sc);
            }
            if (idx_count == 0 && (node_count > 0 || contact_count > 0)) {
                pax_draw_text(&fb, COL_AMBER, FONT, TXT_BODY, 12, list_y0 + 14,
                              "No entries match the active filter — press L to clear");
            }
        }
        xSemaphoreGive(node_mutex);
    }

    int fy_base  = h - footer_h;
    pax_simple_rect(&fb, COL_HEADER, 0, fy_base, w, footer_h);
    pax_simple_rect(&fb, COL_PANEL,  0, fy_base, w, 1);

    int fx = 10;
    int fy_text = fy_base + 6;
    char counts[48];
    snprintf(counts, sizeof(counts), "Nodes:%d  Contacts:%d", node_count, contact_count);
    pax_vec2f csz = pax_text_size(FONT, TXT_BODY, counts);
    pax_draw_text(&fb, COL_WHITE, FONT, TXT_BODY, fx, fy_text, counts);
    fx += (int)csz.x + 20;

    if (node_filter != MESHCORE_DEVICE_ROLE_UNKNOWN) {
        char pill[40];
        snprintf(pill, sizeof(pill), "filter: %s", role_label(node_filter));
        pax_vec2f psz = pax_text_size(FONT, TXT_BODY, pill);
        pax_simple_rect(&fb, COL_AMBER, fx - 6, fy_text - 2, (int)psz.x + 12, TXT_BODY + 4);
        pax_draw_text(&fb, COL_HEADER, FONT, TXT_BODY, fx, fy_text, pill);
        fx += (int)psz.x + 22;
    }

    const char *ctrl = (node_filter == MESHCORE_DEVICE_ROLE_UNKNOWN)
        ? "W/S nav   A:advert   F:fav   L:filter   Q:QR"
        : "L:next   F:fav   A:advert";
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, fx, fy_text + (TXT_BODY - TXT_SMALL) / 2, ctrl);

    if (identity_is_ready()) {
        uint32_t now_ms2 = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        char adv_buf[48];
        if (last_advert_ms == 0) {
            snprintf(adv_buf, sizeof(adv_buf), "advert: pending");
        } else {
            uint32_t age_s = (now_ms2 - last_advert_ms) / 1000;
            snprintf(adv_buf, sizeof(adv_buf), "last advert: %lus ago", (unsigned long)age_s);
        }
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 10, fy_text + TXT_BODY + 6, adv_buf);
    }
}

// ── Wrapped chat-message rendering (shared by DM + Channel views) ────────────
#define MSG_MAX_LINES 8

// Greedy word-wrap of `text` to fit `max_w` px at FONT/TXT_BODY. Fills `out`
// with NUL-terminated lines and returns the count (>=1). A word longer than a
// line is left on its own line (clipped). Safe: `p` always advances, bounded
// by max_lines. Measures with emoji_measure_text so emoji widths count.
static int msg_wrap(const char *text, int max_w, char out[][MAX_MSG_TEXT], int max_lines) {
    int  nl = 0;
    char line[MAX_MSG_TEXT] = {0};
    int  ll = 0;
    const char *p = text;
    while (*p && nl < max_lines) {
        const char *w0 = p;
        while (*p && *p != ' ') p++;   // word
        while (*p == ' ') p++;          // trailing spaces
        int wlen = (int)(p - w0);
        if (wlen > MAX_MSG_TEXT - 1) wlen = MAX_MSG_TEXT - 1;

        char cand[MAX_MSG_TEXT];
        int  copy = wlen;
        if (ll + copy > MAX_MSG_TEXT - 1) copy = MAX_MSG_TEXT - 1 - ll;
        if (ll) memcpy(cand, line, ll);
        memcpy(cand + ll, w0, copy);
        cand[ll + copy] = 0;

        if (ll == 0 || emoji_measure_text(FONT, TXT_BODY, cand) <= max_w) {
            memcpy(line, cand, ll + copy + 1);
            ll += copy;
        } else {
            memcpy(out[nl], line, ll + 1);
            nl++;
            ll = (wlen < MAX_MSG_TEXT) ? wlen : MAX_MSG_TEXT - 1;
            memcpy(line, w0, ll);
            line[ll] = 0;
        }
    }
    if (ll > 0 && nl < max_lines) { memcpy(out[nl], line, ll + 1); nl++; }
    if (nl == 0) { out[0][0] = 0; nl = 1; }
    return nl;
}

// Render a chat ring bottom-up with variable per-message height (wrapped text +
// a tiny metadata line). `*scroll_p` is the 1-based logical index of the
// bottom-most message to show (== count means newest pinned to the bottom);
// it is clamped + written back so the D-pad scroll operates on the real value.
// Caller must hold the ring's mutex.
static void render_msg_list(int w, int list_y0, int list_h, chat_msg_t *msgs,
                            int head, int count, int *scroll_p) {
    if (count == 0) {
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_BODY, 14, list_y0 + 10,
                      "No messages yet. Press T to type.");
        return;
    }
    int sc = *scroll_p;
    if (sc > count) sc = count;
    if (sc < 1)     sc = 1;
    *scroll_p = sc;

    const int line_h  = TXT_BODY + 4;
    const int avail_w = w - 32;
    char lines[MSG_MAX_LINES][MAX_MSG_TEXT];

    pax_clip(&fb, 0, list_y0, w, list_h);
    int y = list_y0 + list_h;
    for (int li = sc - 1; li >= 0 && y > list_y0; li--) {
        int ring = (head - count + li + MAX_CHAT_MSGS * 2) % MAX_CHAT_MSGS;
        chat_msg_t *m = &msgs[ring];
        if (!m->active) continue;

        int nl = msg_wrap(m->text, avail_w, lines, MSG_MAX_LINES);

        char meta[64] = {0};
        {
            char tbuf[12] = {0};
            if (m->timestamp_unix > 0) {
                time_t t = (time_t)m->timestamp_unix; struct tm lt; localtime_r(&t, &lt);
                snprintf(tbuf, sizeof(tbuf), "%02d:%02d", lt.tm_hour, lt.tm_min);
            }
            char hbuf[16] = {0};
            if (!m->is_mine && m->hops != 0xFF) snprintf(hbuf, sizeof(hbuf), "%uh", (unsigned)m->hops);
            const char *ack = NULL;
            if (m->is_mine && m->ack_state == 1)      ack = "...";
            else if (m->is_mine && m->ack_state == 2) ack = "ack";
            int o = 0;
            if (tbuf[0]) o += snprintf(meta + o, sizeof(meta) - o, "%s", tbuf);
            if (hbuf[0]) o += snprintf(meta + o, sizeof(meta) - o, "%s%s", o ? " - " : "", hbuf);
            if (ack)     o += snprintf(meta + o, sizeof(meta) - o, "%s%s", o ? " - " : "", ack);
        }
        int meta_h = TXT_TINY + 2;
        int mh     = nl * line_h + meta_h + 6;

        y -= mh;
        if (y + mh <= list_y0) break;  // fully above the list

        if (m->is_mine) {
            int maxw = 0;
            for (int k = 0; k < nl; k++) {
                int lw = emoji_measure_text(FONT, TXT_BODY, lines[k]);
                if (lw > maxw) maxw = lw;
            }
            int bx = w - maxw - 16;
            if (bx < 16) bx = 16;
            pax_simple_rect(&fb, COL_PANEL, bx - 6, y + 2, maxw + 12, nl * line_h + meta_h + 2);
            for (int k = 0; k < nl; k++) {
                int lw = emoji_measure_text(FONT, TXT_BODY, lines[k]);
                emoji_draw_text(&fb, COL_BLUE, FONT, TXT_BODY, w - lw - 16, y + 4 + k * line_h, lines[k]);
            }
            pax_col_t mc = (m->ack_state == 2) ? COL_GREEN : COL_GRAY;
            const char *ml = meta[0] ? meta : "You";
            pax_vec2f msz = pax_text_size(FONT, TXT_TINY, ml);
            pax_draw_text(&fb, mc, FONT, TXT_TINY, w - (int)msz.x - 16, y + 4 + nl * line_h, ml);
        } else {
            for (int k = 0; k < nl; k++) {
                emoji_draw_text(&fb, COL_WHITE, FONT, TXT_BODY, 14, y + 4 + k * line_h, lines[k]);
            }
            if (meta[0]) {
                pax_draw_text(&fb, COL_GRAY, FONT, TXT_TINY, 14, y + 4 + nl * line_h, meta);
            }
        }
    }
    pax_noclip(&fb);
}

static void render_chat(void) {
    int w  = (int)pax_buf_get_width(&fb);
    int h  = (int)pax_buf_get_height(&fb);

    pax_background(&fb, COL_BG);
    render_tab_bar();

    if (dm_inbox_mode) {
        int inbox_y0 = TAB_BAR_H + 6;
        int footer_h = 36;
        int inbox_h  = h - inbox_y0 - footer_h;
        int row_h    = 56;
        int rows_vis = inbox_h / row_h;
        if (rows_vis < 1) rows_vis = 1;

        // Optional active DM target on top + saved contacts.
        // idx_map entry: -1 = active dm_target row, otherwise contact index.
        int idx_map[MAX_CONTACTS + 1];
        int idx_count = 0;
        bool active_on_top = dm_target_set;
        if (active_on_top) idx_map[idx_count++] = -1;
        if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            for (int i = 0; i < contact_count && idx_count < MAX_CONTACTS + 1; i++) {
                if (active_on_top && memcmp(contacts[i].pub_key, dm_target_pub, MESHCORE_PUB_KEY_SIZE) == 0)
                    continue;
                idx_map[idx_count++] = i;
            }
            xSemaphoreGive(node_mutex);
        }

        if (idx_count == 0) {
            pax_draw_text(&fb, COL_AMBER, FONT, TXT_BODY, 16, inbox_y0 + 18,
                          "No conversations yet");
            pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 16, inbox_y0 + 18 + TXT_BODY + 6,
                          "Open the Nodes tab and press Enter on a contact.");
        } else {
            if (dm_inbox_cursor >= idx_count) dm_inbox_cursor = idx_count - 1;
            if (dm_inbox_cursor < 0)          dm_inbox_cursor = 0;
            if (dm_inbox_cursor < dm_inbox_scroll)              dm_inbox_scroll = dm_inbox_cursor;
            if (dm_inbox_cursor >= dm_inbox_scroll + rows_vis)  dm_inbox_scroll = dm_inbox_cursor - rows_vis + 1;
            int max_scroll = idx_count - rows_vis;
            if (max_scroll < 0) max_scroll = 0;
            if (dm_inbox_scroll > max_scroll) dm_inbox_scroll = max_scroll;
            if (dm_inbox_scroll < 0)          dm_inbox_scroll = 0;

            for (int row = 0; row < rows_vis; row++) {
                int li = row + dm_inbox_scroll;
                if (li >= idx_count) break;
                int  e         = idx_map[li];
                bool is_active = (e == -1);
                bool is_cursor = (li == dm_inbox_cursor);
                const char *name;
                meshcore_device_role_t role;
                if (is_active) {
                    name = dm_target_name;
                    int ci = contact_find(dm_target_pub);
                    role = (ci >= 0) ? (meshcore_device_role_t)contacts[ci].role
                                     : MESHCORE_DEVICE_ROLE_CHAT_NODE;
                } else {
                    name = contacts[e].alias;
                    role = (meshcore_device_role_t)contacts[e].role;
                }

                int y = inbox_y0 + row * row_h;
                if (is_cursor) {
                    pax_simple_rect(&fb, COL_PANEL, 0, y, w, row_h - 2);
                    pax_simple_rect(&fb, COL_ACCENT, 0, y, 5, row_h - 2);
                }
                pax_simple_rect(&fb, COL_PANEL, 12, y + row_h - 1, w - 24, 1);

                int av_x = 18, av_y = y + (row_h - 36) / 2, av_d = 36;
                pax_col_t av_bg = is_active ? COL_AMBER : COL_BLUE;
                pax_simple_rect(&fb, av_bg, av_x, av_y, av_d, av_d);
                char init[2] = {(char)(name[0] ? toupper((unsigned char)name[0]) : '?'), 0};
                pax_vec2f isz = pax_text_size(FONT, TXT_TITLE, init);
                pax_draw_text(&fb, COL_HEADER, FONT, TXT_TITLE,
                              av_x + (av_d - (int)isz.x) / 2,
                              av_y + (av_d - TXT_TITLE) / 2 - 1, init);

                pax_col_t name_col = is_cursor ? COL_WHITE : COL_WHITE;
                int name_x = av_x + av_d + 12;
                pax_draw_text(&fb, name_col, FONT, TXT_BODY, name_x, y + 6, name);

                // Per-contact unread badge (red pill) just right of the name.
                int row_slot   = is_active ? contact_find(dm_target_pub) : e;
                int row_unread = (row_slot >= 0) ? contact_unread[row_slot] : 0;
                if (row_unread > 0) {
                    char ub[8];
                    snprintf(ub, sizeof(ub), "%d", row_unread > 99 ? 99 : row_unread);
                    pax_vec2f nsz = pax_text_size(FONT, TXT_BODY, name);
                    pax_vec2f usz = pax_text_size(FONT, TXT_SMALL, ub);
                    int bw = (int)usz.x + 12;
                    int bx = name_x + (int)nsz.x + 8;
                    int by = y + 6;
                    pax_simple_rect(&fb, COL_RED, bx, by, bw, TXT_SMALL + 4);
                    pax_draw_text(&fb, COL_HEADER, FONT, TXT_SMALL,
                                  bx + 6, by + 2, ub);
                }

                const char *rl = role_label(role);
                char sub[64];
                if (is_active) snprintf(sub, sizeof(sub), "%s  ·  active DM", rl);
                else           snprintf(sub, sizeof(sub), "%s  ·  saved contact", rl);
                pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL,
                              av_x + av_d + 12, y + 6 + TXT_BODY + 4, sub);

                if (is_cursor) {
                    const char *cta = "Enter ›";
                    pax_vec2f sz = pax_text_size(FONT, TXT_SMALL, cta);
                    pax_draw_text(&fb, COL_AMBER, FONT, TXT_SMALL,
                                  w - (int)sz.x - 12, y + (row_h - TXT_SMALL) / 2, cta);
                }
            }

            if (idx_count > rows_vis) {
                char sc[24];
                snprintf(sc, sizeof(sc), "%d/%d", dm_inbox_cursor + 1, idx_count);
                pax_vec2f sz = pax_text_size(FONT, TXT_SMALL, sc);
                pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL,
                              w - (int)sz.x - 10, h - footer_h - TXT_SMALL - 2, sc);
            }
        }

        int fy_base = h - footer_h;
        pax_simple_rect(&fb, COL_HEADER, 0, fy_base, w, footer_h);
        pax_simple_rect(&fb, COL_PANEL,  0, fy_base, w, 1);
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 10, fy_base + (footer_h - TXT_SMALL) / 2,
                      "W/S: nav   Enter: open   D: delete   Tab: next");
        return;
    }

    int input_y = h - CHAT_INPUT_H - FOOTER_H;
    int list_y0 = CHAT_Y0 + 32;
    int list_h  = input_y - list_y0;
    pax_simple_rect(&fb, COL_PANEL, 0, CHAT_Y0, w, 28);
    {
        char hdr[MESHCORE_MAX_NAME_SIZE + 24];
        snprintf(hdr, sizeof(hdr), "‹  %s", dm_target_set ? dm_target_name : "(no target)");
        pax_draw_text(&fb, COL_WHITE, FONT, TXT_BODY, 10, CHAT_Y0 + 4, hdr);
    }

    if (xSemaphoreTake(chat_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        render_msg_list(w, list_y0, list_h, chat_msgs, chat_head, chat_count, &chat_scroll);
        xSemaphoreGive(chat_mutex);
    }

    int iy = input_y;
    pax_simple_rect(&fb, COL_PANEL, 0, iy, w, CHAT_INPUT_H);
    pax_simple_rect(&fb, chat_typing ? COL_ACCENT : COL_AMBER, 0, iy, w, 2);
    if (chat_typing) {
        // Render prefix + input via emoji_draw_text so staged emoji glyphs are
        // visible BEFORE Enter (the input is the staging area for the message).
        char prefix[MESHCORE_MAX_NAME_SIZE + 8];
        snprintf(prefix, sizeof(prefix), "DM %s> ", dm_target_name);
        int ty = iy + (CHAT_INPUT_H - TXT_BODY) / 2;
        int pw = emoji_draw_text(&fb, COL_WHITE, FONT, TXT_BODY, 10, ty, prefix);
        int bw = emoji_draw_text(&fb, COL_WHITE, FONT, TXT_BODY, 10 + pw, ty, chat_input);
        pax_draw_text(&fb, COL_WHITE, FONT, TXT_BODY, 10 + pw + bw, ty, "_");

        char ctr[12];
        snprintf(ctr, sizeof(ctr), "%d/%d", chat_input_len, MAX_INPUT_LEN);
        pax_vec2f csz = pax_text_size(FONT, TXT_SMALL, ctr);
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, w - (int)csz.x - 10, iy + (CHAT_INPUT_H - TXT_SMALL) / 2, ctr);
    } else {
        pax_draw_text(&fb, COL_AMBER, FONT, TXT_SMALL, 10, iy + (CHAT_INPUT_H - TXT_SMALL) / 2,
                      "T: type message");
    }

    int fy = h - FOOTER_H;
    pax_simple_rect(&fb, COL_HEADER, 0, fy, w, FOOTER_H);
    pax_simple_rect(&fb, COL_PANEL, 0, fy, w, 1);
    if (chat_typing) {
        const char *hint = "Enter: send   ESC: cancel   Backspace: delete   ";
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 10, fy + (FOOTER_H - TXT_SMALL) / 2, hint);
        pax_vec2f hsz = pax_text_size(FONT, TXT_SMALL, hint);
        int icon_x = 10 + (int)hsz.x;
        int icon_y = fy + FOOTER_H / 2;
        // Green circle icon matching the physical button.
        pax_outline_circle(&fb, COL_GREEN, icon_x + 6, icon_y, 6);
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL,
                      icon_x + 18, fy + (FOOTER_H - TXT_SMALL) / 2, ": emoji");
    } else {
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 10, fy + (FOOTER_H - TXT_SMALL) / 2,
                      "T: type   W/S: scroll   ESC: back to inbox   Tab: next tab");
    }
}

static void render_channel_list(int w, int h) {
    const int row_h    = 38;
    const int footer_h = FOOTER_H;

    pax_simple_rect(&fb, COL_PANEL, 0, CHAT_Y0, w, 28);
    pax_draw_text(&fb, COL_WHITE, FONT, TXT_BODY, 10, CHAT_Y0 + 4, "Channels");

    int rows_y0 = CHAT_Y0 + 32;
    int rows_h  = h - rows_y0 - footer_h;
    int rows_vis = rows_h / row_h;
    if (rows_vis < 1) rows_vis = 1;

    if (channel_list_cursor < 0)                  channel_list_cursor = 0;
    if (channel_list_cursor >= channel_count)     channel_list_cursor = channel_count - 1;

    int scroll = 0;
    if (channel_list_cursor >= rows_vis) scroll = channel_list_cursor - rows_vis + 1;

    for (int row = 0; row < rows_vis && (row + scroll) < channel_count; row++) {
        int i = row + scroll;
        int y = rows_y0 + row * row_h;
        bool is_sel    = (i == channel_list_cursor);
        bool is_active = (i == active_channel_idx);

        if (is_sel) {
            pax_simple_rect(&fb, COL_PANEL, 0, y, w, row_h - 1);
            pax_simple_rect(&fb, COL_ACCENT, 0, y, 5, row_h - 1);
        }
        pax_col_t name_col = is_sel ? COL_WHITE : COL_GRAY;
        int text_y = y + (row_h - TXT_BODY) / 2;

        // Active marker (filled circle, green) on the left.
        if (is_active) {
            pax_draw_text(&fb, COL_GREEN, FONT, TXT_BODY, 18, text_y, ">");
        }
        pax_draw_text(&fb, name_col, FONT, TXT_BODY, 40, text_y, channels[i].name);

        // Per-channel unread badge (red pill) just right of the name.
        if (channel_unread[i] > 0) {
            char ub[8];
            snprintf(ub, sizeof(ub), "%d", channel_unread[i] > 99 ? 99 : channel_unread[i]);
            pax_vec2f nsz = pax_text_size(FONT, TXT_BODY, channels[i].name);
            pax_vec2f usz = pax_text_size(FONT, TXT_SMALL, ub);
            int bw = (int)usz.x + 12;
            int bx = 40 + (int)nsz.x + 8;
            int by = y + (row_h - (TXT_SMALL + 4)) / 2;
            pax_simple_rect(&fb, COL_RED, bx, by, bw, TXT_SMALL + 4);
            pax_draw_text(&fb, COL_HEADER, FONT, TXT_SMALL, bx + 6, by + 2, ub);
        }

        char meta[24];
        snprintf(meta, sizeof(meta), "0x%02X", channels[i].hash);
        pax_vec2f msz = pax_text_size(FONT, TXT_TINY, meta);
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_TINY, w - (int)msz.x - 14,
                      y + (row_h - TXT_TINY) / 2, meta);
    }

    // Add-channel text-input row (when active, shown below the list).
    if (channel_adding) {
        int iy = h - CHAT_INPUT_H - footer_h;
        pax_simple_rect(&fb, COL_PANEL, 0, iy, w, CHAT_INPUT_H);
        pax_simple_rect(&fb, COL_ACCENT, 0, iy, w, 2);
        char disp[40];
        snprintf(disp, sizeof(disp), "add: %s_", field_edit_buf);
        pax_draw_text(&fb, COL_WHITE, FONT, TXT_BODY, 10,
                      iy + (CHAT_INPUT_H - TXT_BODY) / 2, disp);
    }

    int fy = h - footer_h;
    pax_simple_rect(&fb, COL_HEADER, 0, fy, w, footer_h);
    pax_simple_rect(&fb, COL_PANEL, 0, fy, w, 1);
    const char *hint =
        channel_adding
            ? "Type name (e.g. #nl)   Enter: save   ESC: cancel"
            : (channel_list_cursor == 0
                   ? "W/S: nav   Enter: open   A: add   Tab: next"
                   : "W/S: nav   Enter: open   A: add   D: delete   Tab: next");
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 10,
                  fy + (footer_h - TXT_SMALL) / 2, hint);
}

static void render_channel(void) {
    int w  = (int)pax_buf_get_width(&fb);
    int h  = (int)pax_buf_get_height(&fb);

    pax_background(&fb, COL_BG);
    render_tab_bar();

    if (channel_list_mode) {
        render_channel_list(w, h);
        return;
    }

    // Two-row header: channel name on top, "Region: xx" below (mirrors iPhone
    // MeshCore chat header). Height = 50 to fit TXT_BODY + TXT_SMALL with gap.
    const int hdr_h  = 50;
    pax_simple_rect(&fb, COL_PANEL, 0, CHAT_Y0, w, hdr_h);
    {
        const char *nm = (active_channel_idx >= 0 && active_channel_idx < channel_count)
                             ? channels[active_channel_idx].name : "(no channel)";
        pax_draw_text(&fb, COL_WHITE, FONT, TXT_BODY, 12, CHAT_Y0 + 4, nm);

        char sub[48];
        if (region_scope[0]) {
            snprintf(sub, sizeof(sub), "  Region: %s", region_scope);
        } else {
            snprintf(sub, sizeof(sub), "  Region: (set in Settings)");
        }
        pax_col_t sub_col = region_scope[0] ? COL_GRAY : COL_AMBER;
        pax_draw_text(&fb, sub_col, FONT, TXT_SMALL, 12,
                      CHAT_Y0 + 4 + TXT_BODY + 2, sub);
    }

    int input_y = h - CHAT_INPUT_H - FOOTER_H;
    int list_y0 = CHAT_Y0 + hdr_h + 4;
    int list_h  = input_y - list_y0;
    if (xSemaphoreTake(ch_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        render_msg_list(w, list_y0, list_h, ch_msgs, ch_head, ch_count, &ch_scroll);
        xSemaphoreGive(ch_mutex);
    }

    int iy = input_y;
    pax_simple_rect(&fb, COL_PANEL, 0, iy, w, CHAT_INPUT_H);
    pax_simple_rect(&fb, chat_typing ? COL_ACCENT : COL_GREEN, 0, iy, w, 2);
    if (chat_typing) {
        int ty = iy + (CHAT_INPUT_H - TXT_BODY) / 2;
        int pw = emoji_draw_text(&fb, COL_WHITE, FONT, TXT_BODY, 10, ty, "> ");
        int bw = emoji_draw_text(&fb, COL_WHITE, FONT, TXT_BODY, 10 + pw, ty, chat_input);
        pax_draw_text(&fb, COL_WHITE, FONT, TXT_BODY, 10 + pw + bw, ty, "_");

        char ctr[12];
        snprintf(ctr, sizeof(ctr), "%d/%d", chat_input_len, MAX_INPUT_LEN);
        pax_vec2f csz = pax_text_size(FONT, TXT_SMALL, ctr);
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, w - (int)csz.x - 10, iy + (CHAT_INPUT_H - TXT_SMALL) / 2, ctr);
    } else {
        pax_draw_text(&fb, COL_GREEN, FONT, TXT_SMALL, 10, iy + (CHAT_INPUT_H - TXT_SMALL) / 2,
                      "T: send channel message");
    }

    int fy = h - FOOTER_H;
    pax_simple_rect(&fb, COL_HEADER, 0, fy, w, FOOTER_H);
    pax_simple_rect(&fb, COL_PANEL, 0, fy, w, 1);
    if (chat_typing) {
        const char *hint = "Enter: send   ESC: cancel   Backspace: delete   ";
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 10, fy + (FOOTER_H - TXT_SMALL) / 2, hint);
        pax_vec2f hsz = pax_text_size(FONT, TXT_SMALL, hint);
        int icon_x = 10 + (int)hsz.x;
        int icon_y = fy + FOOTER_H / 2;
        // Green circle icon matching the physical button.
        pax_outline_circle(&fb, COL_GREEN, icon_x + 6, icon_y, 6);
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL,
                      icon_x + 18, fy + (FOOTER_H - TXT_SMALL) / 2, ": emoji");
    } else {
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 10, fy + (FOOTER_H - TXT_SMALL) / 2,
                      "T: type   W/S: scroll   R: clear   ESC: list   Tab: next");
    }
}

// 2x4 emoji picker overlay. Drawn on top of an already-rendered chat view.
// Active state owned by chat module.
static void render_emoji_picker_overlay(void) {
    int w = (int)pax_buf_get_width(&fb);
    int h = (int)pax_buf_get_height(&fb);

    // Panel: bottom strip above the existing chat input/footer.
    const int rows    = 2;
    const int cols    = 4;
    const int cell    = 52;
    const int pad     = 14;
    const int panel_w = cols * cell + 2 * pad;
    const int panel_h = rows * cell + 2 * pad + TXT_SMALL + 6;
    int       panel_x = (w - panel_w) / 2;
    int       panel_y = h - CHAT_INPUT_H - FOOTER_H - panel_h - 4;
    if (panel_y < TAB_BAR_H + 4) panel_y = TAB_BAR_H + 4;

    pax_simple_rect(&fb, COL_HEADER, panel_x, panel_y, panel_w, panel_h);
    pax_simple_rect(&fb, COL_ACCENT, panel_x, panel_y, panel_w, 2);
    pax_draw_text(&fb, COL_AMBER, FONT, TXT_SMALL,
                  panel_x + pad, panel_y + 4, "Pick emoji");

    int grid_x = panel_x + pad;
    int grid_y = panel_y + 6 + TXT_SMALL;

    for (int i = 0; i < EMOJI_COUNT; i++) {
        int r  = i / cols;
        int c  = i % cols;
        int cx = grid_x + c * cell + cell / 2;
        int cy = grid_y + r * cell + cell / 2;
        bool sel = (i == emoji_picker_cursor);
        if (sel) {
            pax_simple_rect(&fb, COL_PANEL,
                            cx - cell / 2 + 2, cy - cell / 2 + 2,
                            cell - 4, cell - 4);
        }
        emoji_draw(i, cx, cy, cell / 2 - 6, &fb);
    }
}

void render(void) {
    // Single-flush model: each render_*() draws into fb but does NOT blit.
    // Overlays (QR, emoji picker) draw on top of the base view, and we blit
    // exactly once at the end so the user never sees the base layer briefly
    // through an overlay swap (the old double-blit caused QR/emoji flicker).
    switch (current_view) {
        case VIEW_NODES:
            render_nodes();
            if (qr_overlay_active) render_qr_overlay();
            break;
        case VIEW_CHAT:    render_chat();    break;
        case VIEW_CHANNEL: render_channel(); break;
        case VIEW_SETTINGS:
        default:           render_settings(); break;
    }
    if (emoji_picker_active && chat_typing &&
        (current_view == VIEW_CHAT || current_view == VIEW_CHANNEL)) {
        render_emoji_picker_overlay();
    }
    blit();
}
