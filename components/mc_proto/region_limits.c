// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "region_limits.h"
#include <ctype.h>
#include <string.h>

// ── EU 863-870 sub-bands (ETSI EN 300 220 V3.2.1, ERC-REC-70-03) ─────────────
// Applies to all EU/EEA countries on the harmonised 863-870 SRD allocation.
// Power is ERP (dipole reference). Permille: 1=0.1%, 10=1%, 100=10%.
static const regulatory_subband_t EU868_SUBBANDS[] = {
    {863.000f, 865.000f, 14, 1, false, 0, "g"},    {865.000f, 868.000f, 14, 10, false, 0, "g1"},
    {868.000f, 868.600f, 14, 10, false, 0, "g1'"}, {868.700f, 869.200f, 14, 1, false, 0, "g2"},
    {869.400f, 869.650f, 27, 100, false, 0, "g3"}, {869.700f, 870.000f, 14, 10, false, 0, "g4"},
};

// ── EU 433 (harmonised SRD) ──────────────────────────────────────────────────
static const regulatory_subband_t EU433_SUBBANDS[] = {
    {433.050f, 434.790f, 10, 100, false, 0, "433"},
};

// ── US/CA/MX 902-928 (FCC Part 15.247) ───────────────────────────────────────
// EIRP cap 30 dBm, no duty cycle. Dwell time 400 ms applies to FHSS (Part
// 15.247(a)(1)); fixed-channel LoRa is typically certified under digital
// modulation provisions (Part 15.247(a)(2)) where the dwell rule does not apply.
// We record 400 ms as informational — UI can warn if user enables FHSS-like
// rapid channel hopping, but for fixed-channel ops the field is moot.
static const regulatory_subband_t US915_SUBBANDS[] = {
    {902.000f, 928.000f, 30, 1000, false, 400, "US915"},
};

// ── AU/NZ 915-928 (AS/NZS 4268, similar to US) ───────────────────────────────
static const regulatory_subband_t ANZ915_SUBBANDS[] = {
    {915.000f, 928.000f, 30, 1000, false, 400, "ANZ915"},
};

// ── JP 920-923 (ARIB STD-T108) ───────────────────────────────────────────────
// Strict: 13-16 dBm EIRP + LBT required (no DC-only operation).
static const regulatory_subband_t JP920_SUBBANDS[] = {
    {920.500f, 923.500f, 13, 100, true, 0, "JP920"},
};

// ── KR 920-923 (KCC) ─────────────────────────────────────────────────────────
static const regulatory_subband_t KR920_SUBBANDS[] = {
    {920.000f, 923.000f, 14, 100, true, 0, "KR920"},
};

// ── IN 865-867 (WPC) ─────────────────────────────────────────────────────────
static const regulatory_subband_t IN865_SUBBANDS[] = {
    {865.000f, 867.000f, 30, 1000, false, 0, "IN865"},
};

// ── RU 864-870 (multiple sub-bands; Roskomnadzor) ────────────────────────────
// Two non-overlapping windows commonly used for unlicensed LoRa.
static const regulatory_subband_t RU864_SUBBANDS[] = {
    {864.000f, 865.000f, 14, 1, false, 0, "RU864"},
    {868.700f, 869.200f, 14, 1, false, 0, "RU869"},
};

// ── Country table ────────────────────────────────────────────────────────────
// Order = picker display order. Start with "Unset" so first-boot users see
// they must choose. Then NL first (home turf), then EU neighbours, then
// rest of EU, then non-EU.
const regulatory_country_t REGION_COUNTRIES[] = {
    // Unset sentinel — no TX should occur until user picks a real country.
    // Sub-band list is empty so any freq match returns NULL.
    {"--", "(Choose region)", POWER_UNIT_ERP, NULL, 0},

    // EU 863-870 countries (harmonised — all use same sub-band table)
    {"NL", "Netherlands", POWER_UNIT_ERP, EU868_SUBBANDS, 6},
    {"BE", "Belgium", POWER_UNIT_ERP, EU868_SUBBANDS, 6},
    {"DE", "Germany", POWER_UNIT_ERP, EU868_SUBBANDS, 6},
    {"AT", "Austria", POWER_UNIT_ERP, EU868_SUBBANDS, 6},
    {"FR", "France", POWER_UNIT_ERP, EU868_SUBBANDS, 6},
    {"CH", "Switzerland", POWER_UNIT_ERP, EU868_SUBBANDS, 6},
    {"UK", "United Kingdom", POWER_UNIT_ERP, EU868_SUBBANDS, 6},
    {"IT", "Italy", POWER_UNIT_ERP, EU868_SUBBANDS, 6},
    {"ES", "Spain", POWER_UNIT_ERP, EU868_SUBBANDS, 6},
    {"PT", "Portugal", POWER_UNIT_ERP, EU868_SUBBANDS, 6},
    {"SE", "Sweden", POWER_UNIT_ERP, EU868_SUBBANDS, 6},
    {"NO", "Norway", POWER_UNIT_ERP, EU868_SUBBANDS, 6},
    {"DK", "Denmark", POWER_UNIT_ERP, EU868_SUBBANDS, 6},
    {"FI", "Finland", POWER_UNIT_ERP, EU868_SUBBANDS, 6},
    {"PL", "Poland", POWER_UNIT_ERP, EU868_SUBBANDS, 6},
    {"CZ", "Czechia", POWER_UNIT_ERP, EU868_SUBBANDS, 6},
    {"IE", "Ireland", POWER_UNIT_ERP, EU868_SUBBANDS, 6},
    {"UA", "Ukraine", POWER_UNIT_ERP, EU868_SUBBANDS, 6},
    {"ZA", "South Africa", POWER_UNIT_ERP, EU868_SUBBANDS, 6},

    // EU 433
    {"EU-433", "EU 433 MHz", POWER_UNIT_ERP, EU433_SUBBANDS, 1},

    // Americas (FCC 902-928)
    {"US", "United States", POWER_UNIT_EIRP, US915_SUBBANDS, 1},
    {"CA", "Canada", POWER_UNIT_EIRP, US915_SUBBANDS, 1},
    {"MX", "Mexico", POWER_UNIT_EIRP, US915_SUBBANDS, 1},

    // APAC
    {"AU", "Australia", POWER_UNIT_EIRP, ANZ915_SUBBANDS, 1},
    {"NZ", "New Zealand", POWER_UNIT_EIRP, ANZ915_SUBBANDS, 1},
    {"JP", "Japan", POWER_UNIT_EIRP, JP920_SUBBANDS, 1},
    {"KR", "South Korea", POWER_UNIT_EIRP, KR920_SUBBANDS, 1},
    {"IN", "India", POWER_UNIT_EIRP, IN865_SUBBANDS, 1},

    {"RU", "Russia", POWER_UNIT_ERP, RU864_SUBBANDS, 2},
};
const int REGION_COUNTRY_COUNT = sizeof(REGION_COUNTRIES) / sizeof(REGION_COUNTRIES[0]);

static int strcasecmp_safe(const char* a, const char* b) {
    while (*a && *b) {
        int da = tolower((unsigned char)*a);
        int db = tolower((unsigned char)*b);
        if (da != db) return da - db;
        a++;
        b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

const regulatory_country_t* region_get_country(const char* iso_code) {
    if (!iso_code || !iso_code[0]) return NULL;
    for (int i = 0; i < REGION_COUNTRY_COUNT; i++) {
        if (strcasecmp_safe(REGION_COUNTRIES[i].country_code, iso_code) == 0) {
            return &REGION_COUNTRIES[i];
        }
    }
    return NULL;
}

const regulatory_country_t* region_get_country_by_index(int idx) {
    if (idx < 0 || idx >= REGION_COUNTRY_COUNT) return NULL;
    return &REGION_COUNTRIES[idx];
}

const regulatory_subband_t* region_match_subband(const regulatory_country_t* country, float freq_mhz) {
    if (!country || !country->subbands) return NULL;
    for (uint8_t i = 0; i < country->n_subbands; i++) {
        const regulatory_subband_t* sb = &country->subbands[i];
        if (freq_mhz >= sb->freq_min_mhz && freq_mhz <= sb->freq_max_mhz) {
            return sb;
        }
    }
    return NULL;
}

int8_t region_effective_power_dbm(const regulatory_country_t* country, int8_t conducted_dbm, int8_t antenna_gain_dbi) {
    if (!country) return conducted_dbm;
    int eff = conducted_dbm + antenna_gain_dbi;
    // ERP is referenced to a half-wave dipole (2.15 dBi gain over isotropic).
    // EIRP is referenced to an isotropic radiator. To convert EIRP→ERP subtract
    // 2.15 dB; to express our (isotropic-based) calc as ERP, do the same.
    if (country->power_unit == POWER_UNIT_ERP) {
        eff -= 2;  // round 2.15 down — conservative for the user
    }
    if (eff < -128) eff = -128;
    if (eff > 127) eff = 127;
    return (int8_t)eff;
}

uint32_t region_dc_budget_ms_per_hour(const regulatory_subband_t* sb) {
    if (!sb) return 0;
    // permille: 1000 = 100% = 3600000 ms / hour
    return ((uint32_t)sb->duty_cycle_permille) * 3600u;
}
