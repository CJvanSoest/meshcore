// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

// The PAX category-grid + drilldown painters were retired in the LVGL-only
// migration; lvgl_ui.c renders Settings now. This file keeps the non-rendering
// single-source registry: the category table, the field table, the save
// dispatch, and the label/value/section formatters the LVGL view reuses.

#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "gps.h"
#include "gps_task.h"
#include "map.h"
#include "nodes.h"
#include "radio.h"
#include "region_limits.h"
#include "render_internal.h"
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
    {FIELD_RADIO_FW, "Identity", "Owner name, advert name, role, radio firmware", false},
    {FIELD_COUNTRY, "Regulatory", "Country, antenna gain, duty cycle", false},
    {FIELD_FREQ, "Radio", "Freq, SF, BW, CR, power, preamble, path hash, ...", false},
    // Advert is reached only via the Home -> Advert tile (the tile drills in
    // directly). Hidden here so the Settings grid doesn't have a duplicate.
    {FIELD_FLOOD_ADVERT_INT, "Advert", "Flood + direct intervals, manual send", true},
    {FIELD_WIFI_SSID, "WiFi", "Slot picker + connect toggle", false},
    {FIELD_BLE_ENABLED, "Bluetooth", "BLE companion radio + pairing code", false},
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
    [FIELD_ROLE]               = {"Role", NULL},
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
    [FIELD_PATH_HASH_SIZE]     = {"Path hash size", NULL},
    // ── Advert ──
    [FIELD_FLOOD_ADVERT_INT]   = {"Flood interval", NULL},
    [FIELD_DIRECT_ADVERT_INT]  = {"Direct interval", NULL},
    [FIELD_SEND_FLOOD_NOW]     = {"Send flood now", NULL},
    [FIELD_SEND_DIRECT_NOW]    = {"Send direct now", NULL},
    // ── WiFi ──
    [FIELD_WIFI_SSID]          = {"SSID", NULL},
    [FIELD_WIFI_STATUS]        = {"Status", NULL},
    [FIELD_WIFI_NETWORK]       = {"Network", save_wifi_prefs},
    [FIELD_WIFI_ENABLED]       = {"Enabled", save_wifi_prefs},
    // ── Bluetooth ──
    [FIELD_BLE_ENABLED]        = {"BLE companion", save_ble_enabled},
    [FIELD_BLE_PIN]            = {"Pairing code", save_ble_pin},
    // ── Region & Location ──
    [FIELD_REGION_SCOPE]       = {"Region scope", save_region_scope},
    [FIELD_GPS_LAT]            = {"GPS latitude", save_gps_coords},
    [FIELD_GPS_LON]            = {"GPS longitude", save_gps_coords},
    [FIELD_GPS_SOURCE]         = {"GPS source", NULL},
    // ── Tracking (live GPS background task) ──
    [FIELD_GPS_PROFILE]        = {"Profile", save_gps_track_prefs},
    [FIELD_GPS_INTERVAL_S]     = {"Poll interval", save_gps_track_prefs},
    [FIELD_GPS_DISTANCE_M]     = {"Commit distance", save_gps_track_prefs},
    [FIELD_MAP_PROFILE]        = {"Style", save_map_profile},
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

// Registry label accessor — the LVGL settings view reads field labels through
// this rather than duplicating s_fields[].
const char* settings_field_label(field_t f) {
    if (f < 0 || f >= FIELD_COUNT) return "";
    return s_fields[f].label ? s_fields[f].label : "";
}

// fmt_field is exposed as settings_field_value() (render_internal.h) so the
// LVGL settings view can reuse the same per-field value formatter. Its
// definition lives near the bottom of this file.

// Optional inline section header drawn above a specific field's row. WiFi,
// HTTPS and Bluetooth are now their own categories, so their headers are
// dropped (the category title already names the subsystem). Region &
// Location still bundles GPS coordinates / live tracking / map style, so it
// keeps the "Tracking" and "Map" sub-headers to separate those groups.
const char* settings_section_above(field_t f) {
    switch (f) {
        case FIELD_GPS_PROFILE:
            return "Tracking";
        case FIELD_MAP_PROFILE:
            return "Map";
        default:
            return NULL;
    }
}

// ── fmt_field: central value-string formatter ──────────────────────────────
// One switch per field — replaces the 8 build_rows_* functions. Adding a new
// settings row means: one entry in s_fields[] (label/save) + one case here
// (value formatting). Labels live in the registry; only the dynamic value
// string is produced here.
void settings_field_value(field_t f, char* out, size_t cap) {
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
            snprintf(out, cap, "press ENTER");
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
                snprintf(out, cap, "(no slots -- use launcher)");
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
        case FIELD_BLE_PIN:
            // Always 6 digits with leading zeros (e.g. "012345") so it matches
            // exactly what the phone prompts for.
            snprintf(out, cap, "%06lu", (unsigned long)ble_pin);
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
