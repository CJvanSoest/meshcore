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
    FIELD_RADIO_FW = 0,      // read-only — SX126x silicon ID (version register) reported by C6
    FIELD_RADIO_FW_APP,      // read-only — hand-maintained tanmatsu-radio app fw label
    FIELD_OWNER,
    FIELD_ADV_NAME,
    // ── Regulatory ──
    FIELD_COUNTRY,           // regulatory country — gates freq/power warnings + DC enforcement
    FIELD_ANTENNA_GAIN,      // dBi — only editable once country is set
    FIELD_DUTY_CYCLE,        // read-only: rolling 1h airtime vs sub-band budget
    // ── Radio (FIELD_FREQ down = needs C6) ──
    FIELD_FREQ,
    FIELD_SF,
    FIELD_BW,
    FIELD_CR,
    FIELD_POWER,
    FIELD_SYNC,
    FIELD_PREAMBLE,
    FIELD_PRESET,
    FIELD_SENSITIVITY,       // RX boost on/off (radio gain vs power-save)
    // ── Network & behavior ──
    FIELD_ADVERT_INT,
    FIELD_ROLE,
    FIELD_PATH_HASH_SIZE,
    // ── Region & location ──
    FIELD_REGION_SCOPE,
    FIELD_GPS_LAT,
    FIELD_GPS_LON,
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

// ── Home tile-grid cursor (VIEW_HOME) ────────────────────────────────────────
// Index into the home tile array (0..HOME_TILE_COUNT-1). Owned + updated by
// input.c, read by render_home.c.
extern int home_cursor;

// QR overlay was opened from the home QR-tile (vs the classic 'Q' shortcut in
// nodes view). When true, closing the overlay returns to VIEW_HOME instead of
// leaving the user stranded on the nodes list.
extern bool qr_from_home;

// Short on-screen status toast (e.g. "Flood advert sent"). The string is empty
// when no toast is active. toast_start_ms is the tick-time the toast became
// visible; render_home auto-clears the text after ~2 seconds.
extern char     toast_text[64];
extern uint32_t toast_start_ms;
