// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
// SPDX-FileContributor: Ilias el Matani <hello@ilias.codes>

#include "settings_nvs.h"

#include <stdio.h>
#include <string.h>

#include "config_types.h"  // gps_/map_ profile enums + globals + labels
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "nvs_helpers.h"

#include "bsp/display.h"
#include "bsp/input.h"
#include "bsp/led.h"

#include "esp_random.h"     // esp_fill_random for the API-key entropy
#include "wifi_connection.h"
#include "wifi_settings.h"  // wifi_settings_get/set, slot-0 persistence

#define NVS_HTTP_API_KEY "http.api_key"

// ── NVS keys — same namespace/keys as launcher so settings are shared ────────
#define NVS_LORA_FREQ       "lora.freq"
#define NVS_LORA_SF         "lora.sf"
#define NVS_LORA_BW         "lora.bandwidth"
#define NVS_LORA_CR         "lora.codingrate"
#define NVS_LORA_POWER      "lora.power"
#define NVS_LORA_ADVERT_INT "lora.advint_s"   // legacy (pre-PR-B): single advert interval
#define NVS_LORA_FLOOD_INT  "lora.fldadv_s"   // periodic flood advert (replaces legacy)
#define NVS_LORA_DIRECT_INT "lora.diradv_s"   // periodic direct advert (new)
#define NVS_SOUND_VOLUME    "snd.vol"
#define NVS_SOUND_DM        "snd.dm"     // uint8: 0 off, 1..N slot
#define NVS_SOUND_CHANNEL   "snd.ch"
#define NVS_SOUND_ERROR     "snd.err"
#define NVS_SOUND_BOOT      "snd.boot"
#define NVS_MAP_LAT         "map.lat_e6"     // i32 — VIEW_MAP centre lat × 1e6
#define NVS_MAP_LON         "map.lon_e6"     // i32 — VIEW_MAP centre lon × 1e6
#define NVS_MAP_ZOOM        "map.zoom"       // u8  — VIEW_MAP zoom level (6..14)
#define NVS_MAP_LOCK        "map.lock"       // u8  — lock-to-position toggle
#define NVS_MAP_PROFILE     "map.profile"    // u8  — map_profile_t (style)
#define NVS_GPS_PROFILE     "gps.profile"    // u8  — gps_profile_t
#define NVS_GPS_INT_S       "gps.int_s"      // u16 — 0 = profile default
#define NVS_GPS_DIST_M      "gps.dist_m"     // u16 — 0 = profile default
#define NVS_LORA_ROLE       "lora.role"
#define NVS_LORA_PATHHASH   "lora.pathhash"
#define NVS_LORA_PREAMBLE   "lora.preamble"
#define NVS_LORA_SYNC       "lora.sync"
#define NVS_LORA_REGION     "lora.region"
#define NVS_LORA_COUNTRY    "lora.country"
#define NVS_LORA_ANT_GAIN   "lora.antgain"
#define NVS_LORA_RX_BOOST   "lora.rxboost"
#define NVS_GPS_LAT         "lora.gps.lat"
#define NVS_GPS_LON         "lora.gps.lon"
// Sentinel marking that GPS NVS values are stored in the current ×1e6 scale.
// Absent OR <2 = legacy / possibly half-migrated state — wipe and require
// user to re-enter once. save_gps_coords always writes this to current value.
#define NVS_GPS_SCALE_VER   "lora.gps.sv"
#define GPS_SCALE_VER_CUR   2
#define NVS_GPS_SRC         "lora.gps.src"
#define NVS_BLE_ENABLED     "ble.en"
#define NVS_BLE_GPS_PREF    "ble.gps.pref"
#define NVS_ADVERT_LOC_POL  "ble.advloc"
#define NVS_UI_DISP_BL      "ui.disp_bl"
#define NVS_UI_KB_BL        "ui.kb_bl"
#define NVS_UI_LED_BR       "ui.led_br"
#define NVS_UI_BLANK_AFTER  "ui.blank_after"
#define UI_DEF_DISP_BL      50
#define UI_DEF_KB_BL        50
#define UI_DEF_LED_BR       5
#define UI_DEF_BLANK_AFTER  0     // off by default — user must opt in

static const char *TAG = "settings";

// c6_available + lora_rx_ok live in main/rx_task: settings_nvs needs them to
// decide whether the C6 is reachable and whether to re-enter RX after a config
// push.
extern bool c6_available;
extern bool lora_rx_ok;

// ── WiFi connect prefs + slot-cache ─────────────────────────────────────────
bool    wifi_enabled = true;
uint8_t wifi_slot    = 0;

#define NVS_WIFI_ENABLED  "wifi.enabled"
#define NVS_WIFI_SLOT     "wifi.slot"

typedef struct { uint8_t idx; char ssid[33]; } wifi_slot_entry_t;
static wifi_slot_entry_t s_wifi_slot_cache[WIFI_SLOTS_SCAN_MAX] = {0};
static int               s_wifi_slot_n = 0;

void wifi_slots_refresh(void) {
    s_wifi_slot_n = 0;
    for (int i = 0; i < WIFI_SLOTS_SCAN_MAX && s_wifi_slot_n < WIFI_SLOTS_SCAN_MAX; i++) {
        wifi_settings_t ws = {0};
        if (wifi_settings_get((uint8_t)i, &ws) == ESP_OK && ws.ssid[0]) {
            s_wifi_slot_cache[s_wifi_slot_n].idx = (uint8_t)i;
            strncpy(s_wifi_slot_cache[s_wifi_slot_n].ssid, ws.ssid, 32);
            s_wifi_slot_cache[s_wifi_slot_n].ssid[32] = '\0';
            s_wifi_slot_n++;
        }
    }
}

int wifi_slots_count(void) { return s_wifi_slot_n; }

uint8_t wifi_slot_idx_at(int list_pos) {
    if (list_pos < 0 || list_pos >= s_wifi_slot_n) return 0;
    return s_wifi_slot_cache[list_pos].idx;
}

const char *wifi_slot_ssid_at(int list_pos) {
    if (list_pos < 0 || list_pos >= s_wifi_slot_n) return "";
    return s_wifi_slot_cache[list_pos].ssid;
}

void load_wifi_prefs(void) {
    nvs_handle_t handle;
    if (nvs_open("system", NVS_READONLY, &handle) != ESP_OK) return;
    uint8_t v;
    if (nvs_get_u8(handle, NVS_WIFI_ENABLED, &v) == ESP_OK) wifi_enabled = (v != 0);
    if (nvs_get_u8(handle, NVS_WIFI_SLOT,    &v) == ESP_OK) wifi_slot    = v;
    nvs_close(handle);
}

void save_wifi_prefs(void) {
    nvs_handle_t handle;
    if (nvs_open("system", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u8(handle, NVS_WIFI_ENABLED, wifi_enabled ? 1 : 0);
    nvs_set_u8(handle, NVS_WIFI_SLOT,    wifi_slot);
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "WiFi prefs saved: enabled=%d slot=%u",
             (int)wifi_enabled, (unsigned)wifi_slot);
    // Apply: tear the current association down then attempt the new one
    // (or stay down, if the user disabled WiFi). Synchronous — the UI's
    // "WiFi: connecting..." toast surfaces just before this kicks off.
    wifi_connection_disconnect();
    if (wifi_enabled && wifi_slots_count() > 0) {
        wifi_connection_connect(wifi_slot, 5);
    }
}

// ── Constants ────────────────────────────────────────────────────────────────
const uint16_t BW_OPTIONS[10] = {7, 10, 15, 20, 31, 41, 62, 125, 250, 500};
const int      BW_COUNT       = (int)(sizeof(BW_OPTIONS) / sizeof(BW_OPTIONS[0]));

const lora_preset_t LORA_PRESETS[4] = {
    {"LR Slow",  11,  31, 8},
    {"LR Std",   10,  62, 6},
    {"MeshCore",  8,  62, 6},
    {"SR Fast",   7, 250, 5},
};
const int LORA_PRESET_COUNT = (int)(sizeof(LORA_PRESETS) / sizeof(LORA_PRESETS[0]));

// ── Live settings state ──────────────────────────────────────────────────────
char                          owner_name[33]       = {0};
char                          lora_advert_name[33] = {0};
char                          region_scope[33]     = {0};
lora_protocol_config_params_t lora_cfg             = {0};
uint16_t                      flood_advert_interval_s  = LORA_DEF_FLOOD_ADVERT_INT;
uint16_t                      direct_advert_interval_s = LORA_DEF_DIRECT_ADVERT_INT;

// Sound prefs: 0 = off; otherwise the WAV-file slot number under
// /sd/meshcore/sounds/<n>.wav. Defaults match the ffmpeg-converted
// Pixabay starter pack we ship: slot 1 = DM tweet, 2 = channel whistle,
// 3 = error, 4 = boot. Boot defaults OFF because every flash-cycle
// would re-fire it.
uint8_t                       sound_volume_pct    = 40;
uint8_t                       sound_dm_slot       = 1;
uint8_t                       sound_channel_slot  = 2;
uint8_t                       sound_error_slot    = 3;
uint8_t                       sound_boot_slot     = 0;
meshcore_device_role_t        lora_role            = MESHCORE_DEVICE_ROLE_CHAT_NODE;
uint8_t                       path_hash_size       = LORA_DEF_PATHHASH;
bool                          gps_position_valid   = false;
int32_t                       gps_lat_e6           = 0;
int32_t                       gps_lon_e6           = 0;
gps_source_t                  gps_last_source      = GPS_SRC_NONE;
bool                          ble_enabled          = true;  // default ON
bool                          ble_gps_pref         = true;  // iPhone "GPS Mode = Enabled"
uint8_t                       advert_loc_policy    = 0;     // ADVERT_LOC_NONE
char                          wifi_ssid[33]        = {0};
char                          wifi_password[65]    = {0};
char                          http_api_key[65]     = {0};
char                          country_code[4]      = "--";
int8_t                        antenna_gain_dbi     = 0;
uint8_t                       display_brightness   = UI_DEF_DISP_BL;
uint8_t                       keyboard_brightness  = UI_DEF_KB_BL;
uint8_t                       led_brightness       = UI_DEF_LED_BR;
uint16_t                      display_blank_after_s = UI_DEF_BLANK_AFTER;

int lora_preset_match(void) {
    for (int i = 0; i < LORA_PRESET_COUNT; i++) {
        if (LORA_PRESETS[i].sf == lora_cfg.spreading_factor &&
            LORA_PRESETS[i].bw == (uint16_t)lora_cfg.bandwidth &&
            LORA_PRESETS[i].cr == lora_cfg.coding_rate) {
            return i;
        }
    }
    return -1;
}

// ── Owner name ───────────────────────────────────────────────────────────────
void load_owner_name(void) {
    nvs_handle_t handle;
    esp_err_t res = nvs_open("system", NVS_READONLY, &handle);
    if (res != ESP_OK) {
        snprintf(owner_name, sizeof(owner_name), "(no NVS)");
        return;
    }
    size_t len = sizeof(owner_name) - 1;
    res = nvs_get_str(handle, "owner.nickname", owner_name, &len);
    if (res != ESP_OK) {
        snprintf(owner_name, sizeof(owner_name), "(not set)");
    }
    nvs_close(handle);
}

void save_owner_name(void) {
    nvs_handle_t handle;
    esp_err_t res = nvs_open("system", NVS_READWRITE, &handle);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "NVS open for write failed: %d", res);
        return;
    }
    nvs_set_str(handle, "owner.nickname", owner_name);
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "Owner name saved: %s", owner_name);
}

// ── LoRa advert name (overrides owner_name in adverts when set) ──────────────
void load_lora_advert_name(void) {
    nvs_handle_t handle;
    if (nvs_open("system", NVS_READONLY, &handle) != ESP_OK) return;
    size_t len = sizeof(lora_advert_name) - 1;
    if (nvs_get_str(handle, "lora.advname", lora_advert_name, &len) != ESP_OK) {
        lora_advert_name[0] = '\0';
    }
    nvs_close(handle);
}

void save_lora_advert_name(void) {
    nvs_handle_t handle;
    if (nvs_open("system", NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "NVS open for write failed (lora.advname)");
        return;
    }
    if (lora_advert_name[0] == '\0') {
        nvs_erase_key(handle, "lora.advname");
    } else {
        nvs_set_str(handle, "lora.advname", lora_advert_name);
    }
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "LoRa advert name saved: %s",
             lora_advert_name[0] ? lora_advert_name : "(cleared)");
}

// ── Regulatory country (ISO 3166-1 alpha-2) ──────────────────────────────────
void load_country_code(void) {
    nvs_handle_t handle;
    if (nvs_open("system", NVS_READONLY, &handle) != ESP_OK) {
        strcpy(country_code, "--");
        return;
    }
    size_t len = sizeof(country_code);
    if (nvs_get_str(handle, NVS_LORA_COUNTRY, country_code, &len) != ESP_OK) {
        strcpy(country_code, "--");
    }
    nvs_close(handle);
}

void save_country_code(void) {
    nvs_handle_t handle;
    if (nvs_open("system", NVS_READWRITE, &handle) != ESP_OK) return;
    if (country_code[0] == '\0' || strcmp(country_code, "--") == 0) {
        nvs_erase_key(handle, NVS_LORA_COUNTRY);
    } else {
        nvs_set_str(handle, NVS_LORA_COUNTRY, country_code);
    }
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "Country code saved: %s", country_code);
}

// ── Antenna gain (dBi) ───────────────────────────────────────────────────────
void load_antenna_gain(void) {
    int8_t v;
    if (nvs_load_i8("system", NVS_LORA_ANT_GAIN, &v)) {
        if (v < -3) v = -3;
        if (v > 15) v = 15;
        antenna_gain_dbi = v;
    }
}

void save_antenna_gain(void) {
    if (nvs_save_i8("system", NVS_LORA_ANT_GAIN, antenna_gain_dbi)) {
        ESP_LOGI(TAG, "Antenna gain saved: %d dBi", (int)antenna_gain_dbi);
    }
}

// ── Region scope ─────────────────────────────────────────────────────────────
void load_region_scope(void) {
    nvs_handle_t handle;
    if (nvs_open("system", NVS_READONLY, &handle) != ESP_OK) {
        region_scope[0] = '\0';
        return;
    }
    size_t len = sizeof(region_scope) - 1;
    if (nvs_get_str(handle, NVS_LORA_REGION, region_scope, &len) != ESP_OK) {
        region_scope[0] = '\0';
    }
    nvs_close(handle);
}

void save_region_scope(void) {
    nvs_handle_t handle;
    if (nvs_open("system", NVS_READWRITE, &handle) != ESP_OK) return;
    if (region_scope[0] == '\0') {
        nvs_erase_key(handle, NVS_LORA_REGION);
    } else {
        nvs_set_str(handle, NVS_LORA_REGION, region_scope);
    }
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "Region scope saved: %s",
             region_scope[0] ? region_scope : "(cleared)");
}

// ── Manual GPS coords ────────────────────────────────────────────────────────
// gps_position_valid follows "both keys present in NVS". Missing either key
// (or both) leaves the advert without a position field. UI clears the position
// by writing zeroed coords + setting valid=false, which deletes the NVS keys.
void load_gps_coords(void) {
    gps_position_valid = false;
    gps_lat_e6         = 0;
    gps_lon_e6         = 0;
    gps_last_source    = GPS_SRC_NONE;

    nvs_handle_t handle;
    if (nvs_open("system", NVS_READONLY, &handle) != ESP_OK) return;
    int32_t lat = 0, lon = 0;
    bool have_lat = (nvs_get_i32(handle, NVS_GPS_LAT, &lat) == ESP_OK);
    bool have_lon = (nvs_get_i32(handle, NVS_GPS_LON, &lon) == ESP_OK);
    uint8_t scale_ver = 0;
    nvs_get_u8(handle, NVS_GPS_SCALE_VER, &scale_ver);
    uint8_t src = GPS_SRC_NONE;
    nvs_get_u8(handle, NVS_GPS_SRC, &src);
    nvs_close(handle);

    if (!have_lat || !have_lon) return;

    if (scale_ver < GPS_SCALE_VER_CUR) {
        // Legacy state or half-migrated (lat fixed, lon left in ×1e7) — both
        // look "valid ×1e6" but value can't be trusted. Wipe and force re-entry.
        ESP_LOGW(TAG, "GPS NVS scale_ver=%u < %u — clearing for re-entry",
                 (unsigned)scale_ver, (unsigned)GPS_SCALE_VER_CUR);
        nvs_handle_t rw;
        if (nvs_open("system", NVS_READWRITE, &rw) == ESP_OK) {
            nvs_erase_key(rw, NVS_GPS_LAT);
            nvs_erase_key(rw, NVS_GPS_LON);
            nvs_set_u8(rw, NVS_GPS_SCALE_VER, GPS_SCALE_VER_CUR);
            nvs_commit(rw);
            nvs_close(rw);
        }
        return;
    }

    gps_lat_e6         = lat;
    gps_lon_e6         = lon;
    gps_position_valid = true;
    // Tolerate forward-incompat values: anything > HTTP keeps GPS_SRC_NONE so
    // we don't show garbled labels if a newer firmware wrote a source enum we
    // don't recognise yet.
    if (src <= GPS_SRC_HTTP) gps_last_source = (gps_source_t)src;
}

void save_gps_coords(void) {
    // Source must reflect the active position. Cleared coords always read back
    // as GPS_SRC_NONE so the UI doesn't show a stale "PA1010D" tag next to
    // "(not set)".
    if (!gps_position_valid) gps_last_source = GPS_SRC_NONE;

    nvs_handle_t handle;
    if (nvs_open("system", NVS_READWRITE, &handle) != ESP_OK) return;
    if (gps_position_valid) {
        nvs_set_i32(handle, NVS_GPS_LAT, gps_lat_e6);
        nvs_set_i32(handle, NVS_GPS_LON, gps_lon_e6);
        nvs_set_u8(handle, NVS_GPS_SRC, (uint8_t)gps_last_source);
    } else {
        nvs_erase_key(handle, NVS_GPS_LAT);
        nvs_erase_key(handle, NVS_GPS_LON);
        nvs_erase_key(handle, NVS_GPS_SRC);
    }
    nvs_set_u8(handle, NVS_GPS_SCALE_VER, GPS_SCALE_VER_CUR);
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "GPS coords saved: %s (lat=%ld lon=%ld src=%u)",
             gps_position_valid ? "valid" : "(cleared)",
             (long)gps_lat_e6, (long)gps_lon_e6, (unsigned)gps_last_source);
}

// ── BLE companion on/off ────────────────────────────────────────────────────
// Missing key reads as default (true) so a fresh device boots advertising.
void load_ble_enabled(void) {
    uint8_t v;
    if (nvs_load_u8("system", NVS_BLE_ENABLED, &v)) ble_enabled = (v != 0);
}

void save_ble_enabled(void) {
    if (nvs_save_u8("system", NVS_BLE_ENABLED, ble_enabled ? 1 : 0)) {
        ESP_LOGI(TAG, "BLE enabled saved: %s", ble_enabled ? "on" : "off");
    }
}

// ── HTTP API key (shared secret for the /ping endpoint) ─────────────────────
static void fill_random_hex(char *out_64chars_plus_nul) {
    uint8_t raw[32];
    esp_fill_random(raw, sizeof(raw));
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(raw); i++) {
        out_64chars_plus_nul[2 * i]     = hex[raw[i] >> 4];
        out_64chars_plus_nul[2 * i + 1] = hex[raw[i] & 0x0F];
    }
    out_64chars_plus_nul[64] = '\0';
}

void load_or_init_http_api_key(void) {
    nvs_handle_t handle;
    if (nvs_open("system", NVS_READWRITE, &handle) != ESP_OK) {
        // NVS dead -- generate ephemeral so /ping isn't open
        fill_random_hex(http_api_key);
        ESP_LOGW(TAG, "NVS open failed; using ephemeral API key");
        return;
    }
    size_t len = sizeof(http_api_key);
    esp_err_t r = nvs_get_str(handle, NVS_HTTP_API_KEY, http_api_key, &len);
    if (r != ESP_OK || http_api_key[0] == '\0') {
        fill_random_hex(http_api_key);
        nvs_set_str(handle, NVS_HTTP_API_KEY, http_api_key);
        nvs_commit(handle);
        ESP_LOGI(TAG, "HTTP API key auto-generated + persisted");
    } else {
        ESP_LOGI(TAG, "HTTP API key loaded from NVS");
    }
    nvs_close(handle);
    // Dev convenience: emit the full key on boot so testers can grab it
    // for curl / MeshMapper config without poking around Settings. Acceptable
    // because the serial console is local-only on these dev boards; a
    // production build should drop this line.
    ESP_LOGI(TAG, "HTTP API key (full): %s", http_api_key);
}

void regenerate_http_api_key(void) {
    fill_random_hex(http_api_key);
    nvs_handle_t handle;
    if (nvs_open("system", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_str(handle, NVS_HTTP_API_KEY, http_api_key);
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "HTTP API key regenerated + persisted");
}

// ── BLE GPS preference (iPhone companion-app GPS Mode toggle) ───────────────
void load_ble_gps_pref(void) {
    uint8_t v;
    if (nvs_load_u8("system", NVS_BLE_GPS_PREF, &v)) ble_gps_pref = (v != 0);
}

void save_ble_gps_pref(void) {
    if (nvs_save_u8("system", NVS_BLE_GPS_PREF, ble_gps_pref ? 1 : 0)) {
        ESP_LOGI(TAG, "BLE gps pref saved: %s", ble_gps_pref ? "on" : "off");
    }
}

// ── Advert location policy (iPhone "Share my position" toggle) ──────────────
void load_advert_loc_policy(void) {
    uint8_t v;
    if (nvs_load_u8("system", NVS_ADVERT_LOC_POL, &v)) advert_loc_policy = v;
}

void save_advert_loc_policy(void) {
    if (nvs_save_u8("system", NVS_ADVERT_LOC_POL, advert_loc_policy)) {
        ESP_LOGI(TAG, "Advert loc policy saved: %u", (unsigned)advert_loc_policy);
    }
}

// ── WiFi creds (delegated to wifi-manager's slot 0 NVS) ─────────────────────
void load_wifi(void) {
    wifi_settings_t ws = {0};
    if (wifi_settings_get(0, &ws) == ESP_OK) {
        strncpy(wifi_ssid,     ws.ssid,     sizeof(wifi_ssid)     - 1);
        strncpy(wifi_password, ws.password, sizeof(wifi_password) - 1);
        wifi_ssid[sizeof(wifi_ssid) - 1] = '\0';
        wifi_password[sizeof(wifi_password) - 1] = '\0';
    } else {
        wifi_ssid[0]     = '\0';
        wifi_password[0] = '\0';
    }
    ESP_LOGI(TAG, "WiFi creds loaded: ssid=\"%s\"", wifi_ssid);
}

// ── LoRa config ──────────────────────────────────────────────────────────────
void load_lora_from_nvs(void) {
    lora_cfg.frequency                  = LORA_DEF_FREQ;
    lora_cfg.spreading_factor           = LORA_DEF_SF;
    lora_cfg.bandwidth                  = LORA_DEF_BW;
    lora_cfg.coding_rate                = LORA_DEF_CR;
    lora_cfg.power                      = LORA_DEF_POWER;
    lora_cfg.sync_word                  = LORA_DEF_SYNC;
    lora_cfg.preamble_length            = LORA_DEF_PREAMBLE;
    lora_cfg.ramp_time                  = LORA_DEF_RAMP;
    lora_cfg.crc_enabled                = true;
    lora_cfg.invert_iq                  = false;
    lora_cfg.low_data_rate_optimization = false;
    lora_cfg.rx_boost                   = true;  // boosted RX by default (+3 dB)

    nvs_handle_t handle;
    if (nvs_open("system", NVS_READONLY, &handle) != ESP_OK) return;
    uint32_t freq = 0;
    uint8_t  sf = 0, cr = 0, power = 0;
    uint16_t bw = 0;
    if (nvs_get_u32(handle, NVS_LORA_FREQ,  &freq)  == ESP_OK && freq  != 0) lora_cfg.frequency        = freq;
    if (nvs_get_u8 (handle, NVS_LORA_SF,    &sf)    == ESP_OK && sf    != 0) lora_cfg.spreading_factor = sf;
    if (nvs_get_u16(handle, NVS_LORA_BW,    &bw)    == ESP_OK && bw    != 0) lora_cfg.bandwidth        = bw;
    if (nvs_get_u8 (handle, NVS_LORA_CR,    &cr)    == ESP_OK && cr    != 0) lora_cfg.coding_rate      = cr;
    if (nvs_get_u8 (handle, NVS_LORA_POWER, &power) == ESP_OK)               lora_cfg.power            = power;
    // Advert intervals (PR-B): two separate fields. Migrate the legacy
    // single advert-interval key one-shot into the new flood key when no
    // new-key value is yet present, so an upgrading badge keeps its old
    // setting until the user explicitly retunes via the Advert menu.
    uint16_t fld = 0, dir = 0, legacy = 0;
    bool have_fld = (nvs_get_u16(handle, NVS_LORA_FLOOD_INT,  &fld) == ESP_OK);
    bool have_dir = (nvs_get_u16(handle, NVS_LORA_DIRECT_INT, &dir) == ESP_OK);
    bool have_lgy = (nvs_get_u16(handle, NVS_LORA_ADVERT_INT, &legacy) == ESP_OK);
    if (have_fld) {
        flood_advert_interval_s = fld;
    } else if (have_lgy) {
        flood_advert_interval_s = legacy;   // one-shot migration
    }
    if (have_dir) {
        direct_advert_interval_s = dir;
    }
    uint8_t role = 0;
    if (nvs_get_u8(handle, NVS_LORA_ROLE, &role) == ESP_OK && role <= MESHCORE_DEVICE_ROLE_SENSOR) {
        lora_role = (meshcore_device_role_t)role;
    }
    uint8_t phs = 0;
    if (nvs_get_u8(handle, NVS_LORA_PATHHASH, &phs) == ESP_OK && phs >= 1 && phs <= 3) {
        path_hash_size = phs;
    }
    uint8_t rxb = 0;
    if (nvs_get_u8(handle, NVS_LORA_RX_BOOST, &rxb) == ESP_OK) {
        lora_cfg.rx_boost = (rxb != 0);
    }
    uint16_t pre = 0;
    if (nvs_get_u16(handle, NVS_LORA_PREAMBLE, &pre) == ESP_OK && pre != 0) {
        lora_cfg.preamble_length = pre;
    }
    uint8_t sync = 0;
    if (nvs_get_u8(handle, NVS_LORA_SYNC, &sync) == ESP_OK && sync != 0) {
        lora_cfg.sync_word = sync;
    }
    nvs_close(handle);
}

void save_lora_to_nvs(void) {
    nvs_handle_t handle;
    if (nvs_open("system", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u32(handle, NVS_LORA_FREQ,  lora_cfg.frequency);
    nvs_set_u8 (handle, NVS_LORA_SF,    lora_cfg.spreading_factor);
    nvs_set_u16(handle, NVS_LORA_BW,    (uint16_t)lora_cfg.bandwidth);
    nvs_set_u8 (handle, NVS_LORA_CR,    lora_cfg.coding_rate);
    nvs_set_u8 (handle, NVS_LORA_POWER, lora_cfg.power);
    nvs_set_u16(handle, NVS_LORA_FLOOD_INT,  flood_advert_interval_s);
    nvs_set_u16(handle, NVS_LORA_DIRECT_INT, direct_advert_interval_s);
    nvs_set_u8 (handle, NVS_LORA_ROLE,  (uint8_t)lora_role);
    nvs_set_u8 (handle, NVS_LORA_PATHHASH, path_hash_size);
    nvs_set_u8 (handle, NVS_LORA_RX_BOOST, lora_cfg.rx_boost ? 1 : 0);
    nvs_set_u16(handle, NVS_LORA_PREAMBLE, lora_cfg.preamble_length);
    nvs_set_u8 (handle, NVS_LORA_SYNC,     lora_cfg.sync_word);
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "LoRa config saved to NVS");
}

// load_lora_config()/save_lora_config() — which reconcile lora_cfg with the C6
// over lora_handle — moved to radio.c so this L1 config store no longer reaches
// up into the radio layer. The pure-NVS halves (load_lora_from_nvs /
// save_lora_to_nvs above) stay here.

// ── Brightness (display backlight, keyboard backlight, RGB LED) ──────────────
// Per-app values that override the launcher's globals while MeshCore is
// running. Apply on change; restore-on-exit is best-effort (skipped for now).
static uint8_t clamp_pct(int v) {
    if (v < 0)   return 0;
    if (v > 100) return 100;
    return (uint8_t)v;
}

void load_brightness(void) {
    nvs_handle_t handle;
    if (nvs_open("system", NVS_READONLY, &handle) != ESP_OK) return;
    uint8_t v;
    if (nvs_get_u8(handle, NVS_UI_DISP_BL, &v) == ESP_OK) display_brightness  = clamp_pct(v);
    if (nvs_get_u8(handle, NVS_UI_KB_BL,   &v) == ESP_OK) keyboard_brightness = clamp_pct(v);
    if (nvs_get_u8(handle, NVS_UI_LED_BR,  &v) == ESP_OK) led_brightness      = clamp_pct(v);
    uint16_t v16;
    if (nvs_get_u16(handle, NVS_UI_BLANK_AFTER, &v16) == ESP_OK) display_blank_after_s = v16;
    nvs_close(handle);
}

void save_brightness(void) {
    nvs_handle_t handle;
    if (nvs_open("system", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u8 (handle, NVS_UI_DISP_BL, display_brightness);
    nvs_set_u8 (handle, NVS_UI_KB_BL,   keyboard_brightness);
    nvs_set_u8 (handle, NVS_UI_LED_BR,  led_brightness);
    nvs_set_u16(handle, NVS_UI_BLANK_AFTER, display_blank_after_s);
    nvs_commit(handle);
    nvs_close(handle);
}

void apply_brightness(void) {
    bsp_display_set_backlight_brightness(display_brightness);
    bsp_input_set_backlight_brightness(keyboard_brightness);
    bsp_led_set_brightness(led_brightness);
}

// ── Sound prefs ─────────────────────────────────────────────────────────────
// Volume in NVS as uint8 %, flags in NVS as a uint8 bitmask:
//   bit0 = DM enabled, bit1 = Channel, bit2 = Error, bit3 = Boot.
void load_sound_prefs(void) {
    nvs_handle_t handle;
    if (nvs_open("system", NVS_READONLY, &handle) != ESP_OK) return;
    uint8_t v;
    if (nvs_get_u8(handle, NVS_SOUND_VOLUME,  &v) == ESP_OK) sound_volume_pct  = clamp_pct(v);
    if (nvs_get_u8(handle, NVS_SOUND_DM,      &v) == ESP_OK) sound_dm_slot     = v;
    if (nvs_get_u8(handle, NVS_SOUND_CHANNEL, &v) == ESP_OK) sound_channel_slot= v;
    if (nvs_get_u8(handle, NVS_SOUND_ERROR,   &v) == ESP_OK) sound_error_slot  = v;
    if (nvs_get_u8(handle, NVS_SOUND_BOOT,    &v) == ESP_OK) sound_boot_slot   = v;
    nvs_close(handle);
}

void save_sound_prefs(void) {
    nvs_handle_t handle;
    if (nvs_open("system", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u8(handle, NVS_SOUND_VOLUME,  sound_volume_pct);
    nvs_set_u8(handle, NVS_SOUND_DM,      sound_dm_slot);
    nvs_set_u8(handle, NVS_SOUND_CHANNEL, sound_channel_slot);
    nvs_set_u8(handle, NVS_SOUND_ERROR,   sound_error_slot);
    nvs_set_u8(handle, NVS_SOUND_BOOT,    sound_boot_slot);
    nvs_commit(handle);
    nvs_close(handle);
}

// ── Map view state (Phase 3 + 6) ────────────────────────────────────────────
bool load_map_state(int32_t *lat_e6, int32_t *lon_e6, uint8_t *zoom) {
    nvs_handle_t handle;
    if (nvs_open("system", NVS_READONLY, &handle) != ESP_OK) return false;
    int32_t lat = 0, lon = 0;
    uint8_t z   = 0;
    uint8_t lock = 1;
    bool ok =
        nvs_get_i32(handle, NVS_MAP_LAT,  &lat) == ESP_OK &&
        nvs_get_i32(handle, NVS_MAP_LON,  &lon) == ESP_OK &&
        nvs_get_u8 (handle, NVS_MAP_ZOOM, &z)   == ESP_OK;
    // Lock key is optional — falls back to the runtime default if missing.
    if (nvs_get_u8 (handle, NVS_MAP_LOCK, &lock) == ESP_OK) {
        map_lock_on = (lock != 0);
    }
    nvs_close(handle);
    if (!ok) return false;
    if (lat_e6) *lat_e6 = lat;
    if (lon_e6) *lon_e6 = lon;
    if (zoom)   *zoom   = z;
    return true;
}

void save_map_state(int32_t lat_e6, int32_t lon_e6, uint8_t zoom) {
    nvs_handle_t handle;
    if (nvs_open("system", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_i32(handle, NVS_MAP_LAT,  lat_e6);
    nvs_set_i32(handle, NVS_MAP_LON,  lon_e6);
    nvs_set_u8 (handle, NVS_MAP_ZOOM, zoom);
    nvs_set_u8 (handle, NVS_MAP_LOCK, map_lock_on ? 1 : 0);
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "Map state saved: lat=%ld lon=%ld zoom=%u lock=%d",
             (long)lat_e6, (long)lon_e6, (unsigned)zoom, (int)map_lock_on);
}

// ── Map style profile ───────────────────────────────────────────────────────
void load_map_profile(void) {
    nvs_handle_t handle;
    if (nvs_open("system", NVS_READONLY, &handle) != ESP_OK) return;
    uint8_t p = (uint8_t)map_profile;
    if (nvs_get_u8(handle, NVS_MAP_PROFILE, &p) == ESP_OK &&
        p < MAP_PROFILE_COUNT) {
        map_profile = (map_profile_t)p;
    }
    nvs_close(handle);
}

void save_map_profile(void) {
    nvs_handle_t handle;
    if (nvs_open("system", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u8(handle, NVS_MAP_PROFILE, (uint8_t)map_profile);
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "Map profile saved: %s", map_profile_label(map_profile));
}

// ── Live GPS tracking prefs (Phase 4) ────────────────────────────────────────
void load_gps_track_prefs(void) {
    nvs_handle_t handle;
    if (nvs_open("system", NVS_READONLY, &handle) != ESP_OK) return;
    uint8_t  p = (uint8_t)gps_profile;
    uint16_t i = gps_custom_interval_s;
    uint16_t d = gps_custom_distance_m;
    if (nvs_get_u8 (handle, NVS_GPS_PROFILE, &p) == ESP_OK &&
        p < GPS_PROFILE_COUNT) {
        gps_profile = (gps_profile_t)p;
    }
    if (nvs_get_u16(handle, NVS_GPS_INT_S,   &i) == ESP_OK) gps_custom_interval_s = i;
    if (nvs_get_u16(handle, NVS_GPS_DIST_M,  &d) == ESP_OK) gps_custom_distance_m = d;
    nvs_close(handle);
}

void save_gps_track_prefs(void) {
    nvs_handle_t handle;
    if (nvs_open("system", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u8 (handle, NVS_GPS_PROFILE, (uint8_t)gps_profile);
    nvs_set_u16(handle, NVS_GPS_INT_S,   gps_custom_interval_s);
    nvs_set_u16(handle, NVS_GPS_DIST_M,  gps_custom_distance_m);
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "GPS track prefs: profile=%s int=%us dist=%um",
             gps_profile_label(gps_profile),
             (unsigned)gps_custom_interval_s, (unsigned)gps_custom_distance_m);
}
