// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

// ── Settings tab: field identifiers (also drives FIELD_COUNT for nav wrap) ───
// Enum order == display order in the Settings tab (render walks it top to bottom
// and inserts section headings at the boundaries below). Everything from
// FIELD_FREQ onward is a radio-config field that needs the C6 — render greys
// those out when the radio is unavailable, so FIELD_FREQ must stay that boundary.
typedef enum {
    // ── Device & identity ──
    FIELD_RADIO_FW = 0,  // read-only — SX126x silicon ID (version register) reported by C6
    FIELD_RADIO_FW_APP,  // read-only — hand-maintained tanmatsu-radio app fw label
    FIELD_OWNER,
    FIELD_ADV_NAME,
    // ── Regulatory ──
    FIELD_COUNTRY,       // regulatory country — gates freq/power warnings + DC enforcement
    FIELD_ANTENNA_GAIN,  // dBi — only editable once country is set
    FIELD_DUTY_CYCLE,    // read-only: rolling 1h airtime vs sub-band budget
    // ── Radio (FIELD_FREQ down = needs C6) ──
    FIELD_FREQ,
    FIELD_SF,
    FIELD_BW,
    FIELD_CR,
    FIELD_POWER,
    FIELD_SYNC,
    FIELD_PREAMBLE,
    FIELD_PRESET,
    FIELD_SENSITIVITY,  // RX boost on/off (radio gain vs power-save)
    // ── Advert (own category — flood + direct intervals, manual sends) ──
    FIELD_FLOOD_ADVERT_INT,   // periodic flood-route advert; 0 = off (default)
    FIELD_DIRECT_ADVERT_INT,  // periodic direct-route advert; 0 = off
    FIELD_SEND_FLOOD_NOW,     // Action row: emit a single flood advert immediately
    FIELD_SEND_DIRECT_NOW,    // Action row: emit a single direct advert (1-hop) now
    // ── Network & behavior ──
    FIELD_ROLE,
    FIELD_PATH_HASH_SIZE,
    // ── WiFi (lives in the Network tile alongside mesh params).
    //     Slot management (add/edit SSID + password for new networks) lives
    //     in the launcher's WiFi settings; our app only flips the on/off
    //     toggle and lets the user pick which already-stored slot to use.
    FIELD_WIFI_SSID,         // Read-only: SSID of the currently selected slot
    FIELD_WIFI_STATUS,       // Read-only: "Disconnected" / "Connecting..." / "IP: x.x.x.x"
    FIELD_WIFI_NETWORK,      // Picker: cycles launcher-stored slots (idx + SSID)
    FIELD_WIFI_ENABLED,      // Toggle: On = wifi_connection_connect, Off = disconnect
    FIELD_HTTP_URL,          // Read-only: https://<ip>:8443/ping (for MeshMapper config)
    FIELD_HTTP_API_KEY,      // Read-only: 64 hex chars (paste into MeshMapper)
    FIELD_HTTP_KEY_REGEN,    // Action row: press OK to roll a new API key
    FIELD_HTTPS_CERT_FP,     // Read-only: SHA-256 fingerprint of the on-device cert
    FIELD_HTTPS_CERT_REGEN,  // Action row: wipe NVS cert + regenerate self-signed
    FIELD_HTTP_QR,           // Action row: open QR overlay with /ping URL + key for iPhone capture
    FIELD_BLE_ENABLED,       // Toggle: BLE companion radio on/off (takes effect on next app start)
    // ── Region & location ──
    FIELD_REGION_SCOPE,
    FIELD_GPS_LAT,
    FIELD_GPS_LON,
    FIELD_GPS_SOURCE,    // Read-only: shows whether coords are from PA1010D / Manual / CDC / BLE
    FIELD_GPS_AUTOFILL,  // Action-row: press OK to scan PA1010D on QWIIC and auto-fill lat/lon
    // ── Live GPS tracking (background gps_task) ──
    FIELD_GPS_PROFILE,     // Walking / Cycling / Driving / Manual
    FIELD_GPS_INTERVAL_S,  // Custom poll interval seconds; 0 = use profile default
    FIELD_GPS_DISTANCE_M,  // Custom commit distance metres; 0 = use profile default
    // ── Map style (VIEW_MAP tile profile) ──
    FIELD_MAP_PROFILE,  // Ripple / OSM Bright / CyclOSM / OpenTopo — picks the SD subdir
    // ── Brightness (display backlight, keyboard backlight, RGB LED) ──
    FIELD_DISPLAY_BL,
    FIELD_KB_BL,
    FIELD_LED_BR,
    FIELD_BLANK_AFTER,
    // ── Sounds (notification beeps) ──
    FIELD_SOUND_VOLUME,
    FIELD_SOUND_DM,
    FIELD_SOUND_CHANNEL,
    FIELD_SOUND_ERROR,
    FIELD_SOUND_BOOT,
    FIELD_SOUND_TEST_DM,       // Action: preview DM sound
    FIELD_SOUND_TEST_CHANNEL,  // Action: preview channel sound
    FIELD_SOUND_TEST_ERROR,    // Action: preview error sound
    FIELD_SOUND_TEST_BOOT,     // Action: preview boot sound
    FIELD_COUNT,
} field_t;

// ── Shared settings tab UI state ─────────────────────────────────────────────
extern int  selected;
extern bool edit_mode;
extern bool dirty;
extern char field_edit_buf[33];
extern int  field_edit_len;
extern bool field_editing_text;
extern int  settings_scroll;

// ── Other shared UI flags ────────────────────────────────────────────────────
extern bool qr_overlay_active;
extern bool time_from_nvs;
extern bool lora_ready;

// QR overlay payload mode. CONTACT encodes the badge's own meshcore://contact/add
// URL (the original use); OWNTRACKS encodes the HTTPS /ping URL + API key so the
// user can scan it with the iPhone Camera app and paste into OwnTracks /
// Shortcuts / MeshMapper without typing 64 hex chars by hand.
typedef enum {
    QR_MODE_CONTACT = 0,
    QR_MODE_OWNTRACKS,
} qr_mode_t;
extern qr_mode_t qr_overlay_mode;

// ── Home tile-grid cursor (VIEW_HOME) ────────────────────────────────────────
// Index into the home tile array (0..HOME_TILE_COUNT-1). Owned + updated by
// input.c, read by render_home.c.
extern int home_cursor;

// QR overlay was opened from the home QR-tile (vs the classic 'Q' shortcut in
// nodes view). When true, closing the overlay returns to VIEW_HOME instead of
// leaving the user stranded on the nodes list.
extern bool qr_from_home;

// QR overlay was opened from a Settings → Network row (currently the OwnTracks
// QR action). When true, closing the overlay leaves the user in VIEW_SETTINGS
// on the same drilldown row instead of bouncing to nodes/home.
extern bool qr_from_settings;

// Settings drilldown state. When category_list_mode is true, render_settings
// draws the list of categories; when false, it drills into category_active
// and renders only those fields (with `selected` clamped to that range).
// category_cursor tracks the focused row in the category list.
extern bool settings_category_list_mode;
extern int  settings_category_cursor;
extern int  settings_category_active;

// Toolbox launcher + packet-log view state. cursor selects a sub-tool on the
// launcher; the log view scrolls a frozen-or-live window of captured frames.
extern int  toolbox_cursor;           // selected sub-tool on the Toolbox launcher
extern int  toolbox_log_scroll;       // rows scrolled back from the newest frame
extern bool toolbox_log_paused;       // freeze the displayed window (capture continues)
extern bool toolbox_log_dissect;      // false = hex dump, true = field dissector
extern int  toolbox_coverage_cursor;  // selected repeater in the coverage test

// Short on-screen status toast (e.g. "Flood advert sent"). The string is empty
// when no toast is active. toast_start_ms is the tick-time the toast became
// visible; render_home auto-clears the text after ~2 seconds.
extern char     toast_text[64];
// Most toasts are 2 s confirmations; the BLE passkey override sets this to
// 60 s so the user has time to read + type on the iPhone before it vanishes.
// Render code reads this; default re-applied when a new toast is queued.
extern uint32_t toast_duration_ms;
extern uint32_t toast_start_ms;
