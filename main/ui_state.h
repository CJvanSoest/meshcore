// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

// ── Settings tab: field identifiers (also drives FIELD_COUNT for nav wrap) ───
typedef enum {
    FIELD_RADIO_FW = 0,      // read-only — SX126x silicon version reported by C6
    FIELD_RADIO_FW_APP,      // read-only — hand-maintained tanmatsu-radio app fw label
    FIELD_OWNER,
    FIELD_ADV_NAME,
    FIELD_FREQ,
    FIELD_SF,
    FIELD_BW,
    FIELD_CR,
    FIELD_POWER,
    FIELD_SYNC,
    FIELD_PREAMBLE,
    FIELD_ADVERT_INT,
    FIELD_PRESET,
    FIELD_ROLE,
    FIELD_PATH_HASH_SIZE,
    FIELD_REGION_SCOPE,
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
extern bool radio_bootloader_mode;
extern bool time_from_nvs;
extern bool lora_ready;
