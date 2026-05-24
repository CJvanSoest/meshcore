// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

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
#define LORA_DEF_PREAMBLE   16
#define LORA_DEF_RAMP       40
#define LORA_DEF_ADVERT_INT 300   // 5 min
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
extern uint16_t                      advert_interval_s;
extern meshcore_device_role_t        lora_role;
extern uint8_t                       path_hash_size;         // 1/2/3 bytes per hop

// ── Load/save ────────────────────────────────────────────────────────────────
void load_owner_name(void);
void save_owner_name(void);
void load_lora_advert_name(void);
void save_lora_advert_name(void);
void load_region_scope(void);
void save_region_scope(void);

void load_lora_from_nvs(void);
void save_lora_to_nvs(void);

// load_lora_config pulls from NVS, then reconciles with whatever the C6 reports.
// save_lora_config writes NVS *and* pushes the new config to the C6 (if present).
void load_lora_config(void);
void save_lora_config(void);
