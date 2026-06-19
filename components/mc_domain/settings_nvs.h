// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
// SPDX-FileContributor: Ilias el Matani <hello@ilias.codes>

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lora.h"
#include "meshcore/packet.h"
#include "meshcore/payload/advert.h"

// ── Defaults (launcher-compatible, used when NVS is empty) ───────────────────
#define LORA_DEF_FREQ       869618000u
#define LORA_DEF_SF         8
#define LORA_DEF_BW         62
#define LORA_DEF_CR         8
#define LORA_DEF_POWER      22
#define LORA_DEF_SYNC       0x12
#define LORA_DEF_PREAMBLE   8     // MeshCore protocol default; was 16 leftover
#define LORA_DEF_RAMP       40
// Both advert intervals default to 0 (off): a freshly-flashed badge stays
// silent until the user opts in via the Advert menu. Avoids the flash-heavy
// dev loop spam-flooding the mesh.
#define LORA_DEF_FLOOD_ADVERT_INT  0  // periodic flood advert; 0 = off
#define LORA_DEF_DIRECT_ADVERT_INT 0  // periodic direct advert; 0 = off (manual-only)
#define LORA_DEF_ROLE       1     // MESHCORE_DEVICE_ROLE_CHAT_NODE
// 1-byte hop hashes by default; bump to 2 once you've verified network-wide.
#define LORA_DEF_PATHHASH   1

// ── SX1262 BW choices (kHz) ──────────────────────────────────────────────────
extern const uint16_t BW_OPTIONS[10];
extern const int      BW_COUNT;

// ── LoRa profile presets ─────────────────────────────────────────────────────
typedef struct {
    const char *name;
    uint8_t     sf;
    uint16_t    bw;
    uint8_t     cr;
} lora_preset_t;

extern const lora_preset_t LORA_PRESETS[4];
extern const int           LORA_PRESET_COUNT;

// -1 if current lora_cfg doesn't match any preset.
int lora_preset_match(void);

// ── Live settings state (loaded from NVS on boot, mutated by Settings tab) ───
extern char                          owner_name[33];
extern char                          lora_advert_name[33];   // empty = use owner_name
extern char                          region_scope[33];       // lowercase, e.g. "nl"
extern lora_protocol_config_params_t lora_cfg;
extern uint16_t                      flood_advert_interval_s;
extern uint16_t                      direct_advert_interval_s;
extern meshcore_device_role_t        lora_role;
extern uint8_t                       path_hash_size;         // 1/2/3 bytes per hop

// Regulatory country (ISO 3166-1 alpha-2 or "--" sentinel). Drives the
// freq/power soft-warn + duty-cycle enforcement in region_limits.c. Default
// "--" until user picks one in Settings; until then no regulatory checks are
// applied (firstboot ergonomics — we don't gate TX, just don't warn either).
extern char                          country_code[4];

// External antenna gain (dBi). Feeds region_effective_power_dbm() so the
// regulatory ERP check accounts for high-gain antennas. Default 0 (stub).
// Range -3..15. Only editable in UI once country_code != "--".
extern int8_t                        antenna_gain_dbi;

// Manual GPS coords (degrees × 1e6 — MeshCore upstream scale, see
// AdvertDataHelpers.h::getLat which divides by 1000000.0).
// Both must be valid to be embedded in adverts; invalid = field absent.
extern bool                          gps_position_valid;
extern int32_t                       gps_lat_e6;
extern int32_t                       gps_lon_e6;

// Where the currently-saved gps_lat_e6/lon_e6 came from. Surfaced in Settings
// so the user can tell at a glance whether the position is a PA1010D fix, a
// manual entry, or a push from a paired companion. Set the desired source
// *before* calling save_gps_coords() — it persists alongside the coords.
typedef enum {
    GPS_SRC_NONE    = 0,   // no position saved (or freshly cleared)
    GPS_SRC_MANUAL  = 1,   // user typed lat/lon into Settings
    GPS_SRC_PA1010D = 2,   // QWIIC GPS module
    GPS_SRC_CDC     = 3,   // pushed via USB-CDC companion frame
    GPS_SRC_BLE     = 4,   // pushed via BLE companion
    GPS_SRC_HTTP    = 5,   // pushed via HTTPS /ping endpoint (MeshMapper wardrive)
} gps_source_t;
extern gps_source_t                  gps_last_source;

// Display / keyboard backlight + RGB LED brightness — per-app values that
// override the launcher's globals while MeshCore is running. Values are 0-100%.
// Defaults: display 50, keyboard 50, LED 5 (per backlog #47).
extern uint8_t display_brightness;
extern uint8_t keyboard_brightness;
extern uint8_t led_brightness;

// Idle timeout in seconds after which the MIPI backlight auto-blanks (LoRa
// RX + LEDs keep running). 0 = disabled. F3 short-press always toggles
// regardless of this value.
extern uint16_t display_blank_after_s;

// BLE companion radio on/off. When false, NimBLE is not brought up at all
// (saves the ~5 mA average that the BT controller + advertising draw).
// Default true so the iPhone MeshCore app finds the device on first boot.
// Toggling takes effect on the next app start, since shutting NimBLE down
// cleanly mid-runtime is non-trivial.
extern bool                          ble_enabled;

// iPhone MeshCore companion-app "GPS Mode" preference. Reported via
// COMPANION_CMD_GET_CUSTOM_VARS as "gps:1"/"gps:0" so the app's Position
// Settings screen reflects the persisted state. Currently informational
// only -- our PA1010D auto-fill is user-triggered from Settings, not a
// background poll, so the toggle doesn't change device behaviour yet.
extern bool                          ble_gps_pref;

// "Share my position in adverts" toggle from the iPhone MeshCore Settings
// panel, mapped to upstream ADVERT_LOC_NONE (0) / ADVERT_LOC_SHARE (1).
// Set via COMPANION_CMD_SET_OTHER_PARAMS and reported in SELF_INFO so the
// app's checkbox state survives reconnects.
extern uint8_t                       advert_loc_policy;

// WiFi credentials for the optional Tanmatsu HTTP/HTTPS endpoint feature
// (MeshMapper POSTs location updates -- PR-2g). Persisted via the
// wifi-manager component's wifi_settings_set(slot 0) helper so the badge
// can re-connect at boot. Mirrored here for the Settings UI text-edit
// rows; on commit, settings_save_wifi() writes the wifi-manager slot.
extern char                          wifi_ssid[33];      // SSID (max 32 + NUL)
extern char                          wifi_password[65];  // WPA2 PSK (max 64 + NUL)

// ── Notification sounds (PR D) ───────────────────────────────────────────────
// User drops .wav files into /sd/meshcore/sounds/. At boot the sounds module
// scans the directory and builds an alphabetically sorted index (up to
// SOUNDS_MAX_SLOTS entries). Each event slot stores 0 = Off or 1..N index
// into that list. Volume is a single 0..100 % shared by all events; the
// codec does the actual mixing.
extern uint8_t sound_volume_pct;
extern uint8_t sound_dm_slot;        // 0=off, 1..N = index into discovered WAV list
extern uint8_t sound_channel_slot;
extern uint8_t sound_error_slot;
extern uint8_t sound_boot_slot;

void load_sound_prefs(void);
void save_sound_prefs(void);

// ── Map view state (Phase 3) ────────────────────────────────────────────────
// Centre lat/lon (1e-6 °) and zoom level for VIEW_MAP. The map module owns
// the runtime state (see map.h); these helpers only handle NVS persistence
// so the boot flow and the debounced save inside the render loop both go
// through the same path. Returns false from load_*() if any key is missing —
// the map module then falls back to its built-in default centre.
bool load_map_state(int32_t *lat_e6, int32_t *lon_e6, uint8_t *zoom);
void save_map_state(int32_t lat_e6, int32_t lon_e6, uint8_t zoom);

// Map style profile (Ripple / OSM Bright / CyclOSM / OpenTopo). The
// runtime enum lives in map.{c,h}; this glues it to NVS.
void load_map_profile(void);
void save_map_profile(void);

// WiFi connect preferences. The launcher owns SSID/password slot storage
// via the wifi-manager component; we only persist the user's intent
// (on/off) plus which launcher slot to prefer.
extern bool    wifi_enabled;        // Auto-connect on boot + while enabled
extern uint8_t wifi_slot;           // Active launcher slot to connect to
void load_wifi_prefs(void);
void save_wifi_prefs(void);

// Cached snapshot of launcher-stored WiFi slots. Rebuilt by
// wifi_slots_refresh() on boot, on every Settings 'R' reload, and just
// before the FIELD_WIFI_NETWORK picker enters edit mode — so newly added
// launcher slots show up without re-launching the app.
#define WIFI_SLOTS_SCAN_MAX 16
int         wifi_slots_count(void);
uint8_t     wifi_slot_idx_at(int list_pos);
const char *wifi_slot_ssid_at(int list_pos);
void        wifi_slots_refresh(void);

// Live GPS tracking prefs (Phase 4): profile + custom interval/distance.
// The runtime variables themselves live in gps_task.{c,h}; these helpers
// just glue them to NVS so the Settings UI doesn't have to.
void load_gps_track_prefs(void);
void save_gps_track_prefs(void);

// Shared secret used by the HTTPS /ping endpoint to authenticate POSTs.
// Auto-generated (64 random hex chars) at first boot if NVS is empty, so
// nobody can push to /ping just by knowing the URL. Surfaced in Settings
// so the user can copy it into MeshMapper. Regenerate via the action
// row in Settings if the secret leaks.
extern char                          http_api_key[65];   // 64 hex chars + NUL

// ── Load/save ────────────────────────────────────────────────────────────────
void load_owner_name(void);
void save_owner_name(void);
void load_lora_advert_name(void);
void save_lora_advert_name(void);
void load_region_scope(void);
void save_region_scope(void);
void load_country_code(void);
void save_country_code(void);
void load_antenna_gain(void);
void save_antenna_gain(void);
void load_gps_coords(void);
void save_gps_coords(void);
void load_ble_enabled(void);
void save_ble_enabled(void);
void load_ble_gps_pref(void);
void save_ble_gps_pref(void);
void load_advert_loc_policy(void);
void save_advert_loc_policy(void);

// Load WiFi creds: pulls from the wifi-manager's NVS slot 0 into
// wifi_ssid / wifi_password so wifi_connect_try_all() can use them.
void load_wifi(void);

// Pull http_api_key from NVS; if missing/empty, generate a fresh one and
// persist it. Idempotent: safe to call multiple times.
void load_or_init_http_api_key(void);

// Force-generate a new API key and save. Used by the Settings "Regenerate"
// action row when the existing key may have leaked.
void regenerate_http_api_key(void);
void load_brightness(void);
void save_brightness(void);
// Apply the current backlight + LED values to the BSP. Safe to call any time.
void apply_brightness(void);

void load_lora_from_nvs(void);
void save_lora_to_nvs(void);

// load_lora_config()/save_lora_config() reconcile NVS with the C6 radio; they
// live in radio.c (declared in radio.h) so this L1 header has no radio dep.
