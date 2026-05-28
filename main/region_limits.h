// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

// Regulatory data for SRD/ISM-band LoRa per country/region.
//
// Sources:
//  - EU 863-870: ETSI EN 300 220 V3.2.1 (2018-06) + ERC-REC-70-03
//  - US 902-928: FCC Part 15.247 / 15.249
//  - Country-to-band mapping: Lansitec LoRaWAN frequency plan reference
//  - Meshtastic firmware src/mesh/RadioInterface.cpp (sanity check only —
//    Meshtastic collapses each region into a single sub-band; we keep
//    full sub-band granularity for EU 863-870)
//
// NOT a substitute for the user reading their local regulations. We
// soft-warn on freq/power overshoot; hard-enforce only on duty cycle.

typedef enum {
    POWER_UNIT_ERP,   // Effective radiated power (dipole reference) — EU/ETSI
    POWER_UNIT_EIRP,  // Effective isotropic radiated power — US/FCC, JP, AU/NZ
} power_unit_t;

typedef struct {
    float    freq_min_mhz;
    float    freq_max_mhz;
    int8_t   max_power_dbm;          // unit per containing country
    uint16_t duty_cycle_permille;    // 1000 = no limit, 100 = 10%, 10 = 1%, 1 = 0.1%
    bool     lbt_alternative;        // LBT may replace duty-cycle (informational)
    uint16_t max_dwell_time_ms;      // 0 = no limit (FCC Part 15.247 FHSS: 400)
    const char *label;               // e.g. "g3" — for diagnostic display
} regulatory_subband_t;

typedef struct {
    const char                  *country_code;   // ISO 3166-1 alpha-2 ("NL", "US")
    const char                  *display_name;   // user-facing label
    power_unit_t                 power_unit;
    const regulatory_subband_t  *subbands;
    uint8_t                      n_subbands;
} regulatory_country_t;

// All known countries — order is presentation order in the picker.
extern const regulatory_country_t REGION_COUNTRIES[];
extern const int                  REGION_COUNTRY_COUNT;

// Lookup by ISO 3166-1 alpha-2 code (case-insensitive). NULL if unknown.
const regulatory_country_t *region_get_country(const char *iso_code);

// Lookup by index 0..REGION_COUNTRY_COUNT-1 (picker UI).
const regulatory_country_t *region_get_country_by_index(int idx);

// Find the sub-band containing freq_mhz. NULL if freq is outside any sub-band
// for this country (which is itself a violation — caller should warn).
const regulatory_subband_t *region_match_subband(
    const regulatory_country_t *country, float freq_mhz);

// Compute ERP/EIRP from conducted power + antenna gain, in the unit the
// country regulates in. Caller compares result against subband->max_power_dbm.
int8_t region_effective_power_dbm(
    const regulatory_country_t *country,
    int8_t conducted_dbm, int8_t antenna_gain_dbi);

// Max allowed on-air time per hour in milliseconds for the given sub-band.
// 100% DC → 3600000 ms. 1% → 36000 ms. 0.1% → 3600 ms. 10% → 360000 ms.
uint32_t region_dc_budget_ms_per_hour(const regulatory_subband_t *sb);
