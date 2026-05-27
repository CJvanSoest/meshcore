// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "settings_nvs.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "radio.h"  // lora_handle (radio v3.0.0 handle-based API)

// ── NVS keys — same namespace/keys as launcher so settings are shared ────────
#define NVS_LORA_FREQ       "lora.freq"
#define NVS_LORA_SF         "lora.sf"
#define NVS_LORA_BW         "lora.bandwidth"
#define NVS_LORA_CR         "lora.codingrate"
#define NVS_LORA_POWER      "lora.power"
#define NVS_LORA_ADVERT_INT "lora.advint_s"
#define NVS_LORA_ROLE       "lora.role"
#define NVS_LORA_PATHHASH   "lora.pathhash"
#define NVS_LORA_REGION     "lora.region"
#define NVS_GPS_LAT         "lora.gps.lat"
#define NVS_GPS_LON         "lora.gps.lon"
// Sentinel marking that GPS NVS values are stored in the current ×1e6 scale.
// Absent OR <2 = legacy / possibly half-migrated state — wipe and require
// user to re-enter once. save_gps_coords always writes this to current value.
#define NVS_GPS_SCALE_VER   "lora.gps.sv"
#define GPS_SCALE_VER_CUR   2

static const char *TAG = "settings";

// c6_available + lora_rx_ok live in main/rx_task: settings_nvs needs them to
// decide whether the C6 is reachable and whether to re-enter RX after a config
// push.
extern bool c6_available;
extern bool lora_rx_ok;

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
uint16_t                      advert_interval_s    = LORA_DEF_ADVERT_INT;
meshcore_device_role_t        lora_role            = MESHCORE_DEVICE_ROLE_CHAT_NODE;
uint8_t                       path_hash_size       = LORA_DEF_PATHHASH;
bool                          gps_position_valid   = false;
int32_t                       gps_lat_e6           = 0;
int32_t                       gps_lon_e6           = 0;

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

    nvs_handle_t handle;
    if (nvs_open("system", NVS_READONLY, &handle) != ESP_OK) return;
    int32_t lat = 0, lon = 0;
    bool have_lat = (nvs_get_i32(handle, NVS_GPS_LAT, &lat) == ESP_OK);
    bool have_lon = (nvs_get_i32(handle, NVS_GPS_LON, &lon) == ESP_OK);
    uint8_t scale_ver = 0;
    nvs_get_u8(handle, NVS_GPS_SCALE_VER, &scale_ver);
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
}

void save_gps_coords(void) {
    nvs_handle_t handle;
    if (nvs_open("system", NVS_READWRITE, &handle) != ESP_OK) return;
    if (gps_position_valid) {
        nvs_set_i32(handle, NVS_GPS_LAT, gps_lat_e6);
        nvs_set_i32(handle, NVS_GPS_LON, gps_lon_e6);
    } else {
        nvs_erase_key(handle, NVS_GPS_LAT);
        nvs_erase_key(handle, NVS_GPS_LON);
    }
    nvs_set_u8(handle, NVS_GPS_SCALE_VER, GPS_SCALE_VER_CUR);
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "GPS coords saved: %s (lat=%ld lon=%ld)",
             gps_position_valid ? "valid" : "(cleared)",
             (long)gps_lat_e6, (long)gps_lon_e6);
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
    uint16_t advint = 0;
    if (nvs_get_u16(handle, NVS_LORA_ADVERT_INT, &advint) == ESP_OK && advint != 0) advert_interval_s = advint;
    uint8_t role = 0;
    if (nvs_get_u8(handle, NVS_LORA_ROLE, &role) == ESP_OK && role <= MESHCORE_DEVICE_ROLE_SENSOR) {
        lora_role = (meshcore_device_role_t)role;
    }
    uint8_t phs = 0;
    if (nvs_get_u8(handle, NVS_LORA_PATHHASH, &phs) == ESP_OK && phs >= 1 && phs <= 3) {
        path_hash_size = phs;
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
    nvs_set_u16(handle, NVS_LORA_ADVERT_INT, advert_interval_s);
    nvs_set_u8 (handle, NVS_LORA_ROLE,  (uint8_t)lora_role);
    nvs_set_u8 (handle, NVS_LORA_PATHHASH, path_hash_size);
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "LoRa config saved to NVS");
}

void load_lora_config(void) {
    load_lora_from_nvs();
    c6_available = false;

    lora_protocol_config_params_t c6_cfg = {0};
    esp_err_t res = lora_get_config(&lora_handle, &c6_cfg);
    if (res == ESP_OK) {
        c6_available = true;
        if (c6_cfg.frequency != 0) {
            lora_cfg = c6_cfg;
            save_lora_to_nvs();
            ESP_LOGI(TAG, "LoRa config from C6: freq=%luHz sf=%d",
                     (unsigned long)lora_cfg.frequency, lora_cfg.spreading_factor);
        } else {
            ESP_LOGI(TAG, "C6 has empty config, pushing NVS values to C6");
            lora_set_config(&lora_handle, &lora_cfg);
        }
    } else {
        ESP_LOGW(TAG, "C6 unavailable (err=%d) — using NVS values", res);
    }
}

void save_lora_config(void) {
    save_lora_to_nvs();
    if (!c6_available) return;
    esp_err_t res = lora_set_config(&lora_handle, &lora_cfg);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "lora_set_config failed: %d", res);
    } else {
        ESP_LOGI(TAG, "LoRa config pushed to C6");
        // lora_set_config resets the radio to standby — re-enter RX so we keep
        // listening on the new frequency/SF.
        if (lora_rx_ok) {
            lora_set_mode(&lora_handle, LORA_PROTOCOL_MODE_RX);
        }
    }
}
