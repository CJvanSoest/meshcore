// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "render.h"
#include "render_internal.h"

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

void render_settings(void) {
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
        if (y + disp[d].hgt <= y0 || y >= y0 + list_h) continue;

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
                    regulatory_violation = true;
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
