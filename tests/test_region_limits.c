// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// Host-side test for the regulatory limits table in main/region_limits.c.
//
// Links against the SAME translation unit the firmware ships. A wrong
// sub-band boundary, a flipped power unit, or a miscomputed duty-cycle budget
// is exactly the class of bug that hands a user an off-band or over-power
// radio config, so it should make CI red before it can merge.
//
// Build (see tests/Makefile):
//     gcc -I../main test_region_limits.c ../main/region_limits.c -o test_region_limits
//
// Exit 0 on pass, 1 on any mismatch.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "region_limits.h"

static int failures = 0;

#define EXPECT(cond, fmt, ...) do {                                            \
    if (!(cond)) {                                                             \
        fprintf(stderr, "FAIL %s:%d: " fmt "\n", __FILE__, __LINE__,           \
                ##__VA_ARGS__);                                                \
        failures++;                                                            \
    }                                                                          \
} while (0)

// Convenience: label of the sub-band a freq maps to, or "(none)".
static const char *band_at(const regulatory_country_t *c, float f) {
    const regulatory_subband_t *sb = region_match_subband(c, f);
    return sb ? sb->label : "(none)";
}

int main(void) {
    // ── Country lookup: case-insensitive, NULL-safe ──────────────────────────
    const regulatory_country_t *nl = region_get_country("NL");
    EXPECT(nl != NULL, "NL must resolve");
    EXPECT(nl && strcmp(nl->display_name, "Netherlands") == 0,
           "NL display_name");
    EXPECT(region_get_country("nl") == nl, "lookup must be case-insensitive");
    EXPECT(nl && nl->power_unit == POWER_UNIT_ERP, "NL regulates in ERP");
    EXPECT(nl && nl->n_subbands == 6, "NL has 6 EU868 sub-bands");

    EXPECT(region_get_country("ZZ") == NULL, "unknown code → NULL");
    EXPECT(region_get_country(NULL) == NULL, "NULL code → NULL");
    EXPECT(region_get_country("")   == NULL, "empty code → NULL");

    // The "(Choose region)" sentinel exists, has no sub-bands, so no TX config
    // can validate against it (first-boot guard).
    const regulatory_country_t *unset = region_get_country("--");
    EXPECT(unset != NULL && unset->n_subbands == 0,
           "unset sentinel present with 0 sub-bands");
    EXPECT(region_match_subband(unset, 869.618f) == NULL,
           "unset sentinel matches no band");

    // ── Index lookup (picker UI) ─────────────────────────────────────────────
    EXPECT(region_get_country_by_index(0) == unset, "index 0 is the sentinel");
    EXPECT(region_get_country_by_index(-1) == NULL, "negative index → NULL");
    EXPECT(region_get_country_by_index(REGION_COUNTRY_COUNT) == NULL,
           "out-of-range index → NULL");
    EXPECT(REGION_COUNTRY_COUNT > 25, "table has the documented ~30 entries");

    // ── EU 868 sub-band matching ─────────────────────────────────────────────
    // Default MeshCore freq 869.618 sits in g3 (the 27 dBm / 10% high band).
    const regulatory_subband_t *g3 = region_match_subband(nl, 869.618f);
    EXPECT(g3 && strcmp(g3->label, "g3") == 0, "869.618 → g3, got %s",
           band_at(nl, 869.618f));
    EXPECT(g3 && g3->max_power_dbm == 27, "g3 max power 27 dBm ERP");
    EXPECT(g3 && g3->duty_cycle_permille == 100, "g3 duty 10%%");

    EXPECT(strcmp(band_at(nl, 866.0f), "g1")  == 0, "866.0 → g1");
    EXPECT(strcmp(band_at(nl, 868.3f), "g1'") == 0, "868.3 → g1'");
    // Gap between g2 (…869.200) and g3 (869.400…): no sub-band covers it.
    EXPECT(region_match_subband(nl, 869.300f) == NULL, "869.3 is an inter-band gap");
    // Well outside the EU 868 allocation entirely.
    EXPECT(region_match_subband(nl, 915.0f) == NULL, "915 is off-band for NL");

    // ── Other regions ────────────────────────────────────────────────────────
    const regulatory_country_t *us = region_get_country("US");
    EXPECT(us && us->power_unit == POWER_UNIT_EIRP, "US regulates in EIRP");
    const regulatory_subband_t *us915 = region_match_subband(us, 915.0f);
    EXPECT(us915 && strcmp(us915->label, "US915") == 0, "915 → US915");
    EXPECT(us915 && us915->max_power_dbm == 30, "US 30 dBm EIRP");
    EXPECT(us915 && us915->duty_cycle_permille == 1000, "US no duty limit");

    const regulatory_country_t *eu433 = region_get_country("EU-433");
    EXPECT(strcmp(band_at(eu433, 433.5f), "433") == 0, "433.5 → 433 band");

    // ── Effective power: ERP subtracts the 2.15 dBi dipole reference
    //    (rounded to 2, conservatively), EIRP does not. ─────────────────────────
    EXPECT(region_effective_power_dbm(nl, 22, 0) == 20, "ERP: 22+0-2 = 20");
    EXPECT(region_effective_power_dbm(nl, 22, 3) == 23, "ERP: 22+3-2 = 23");
    EXPECT(region_effective_power_dbm(us, 22, 0) == 22, "EIRP: 22+0 = 22");
    EXPECT(region_effective_power_dbm(us, 22, 3) == 25, "EIRP: 22+3 = 25");
    EXPECT(region_effective_power_dbm(NULL, 17, 5) == 17,
           "NULL country returns conducted power unchanged");

    // ── Duty-cycle budget: permille × 3600 ms ────────────────────────────────
    EXPECT(region_dc_budget_ms_per_hour(g3)    == 360000u, "10%% → 360000 ms/h");
    EXPECT(region_dc_budget_ms_per_hour(us915) == 3600000u, "100%% → 3600000 ms/h");
    EXPECT(region_dc_budget_ms_per_hour(region_match_subband(nl, 866.0f)) == 36000u,
           "1%% (g1) → 36000 ms/h");
    EXPECT(region_dc_budget_ms_per_hour(NULL) == 0u, "NULL sub-band → 0 budget");

    // ── Table integrity: every real country has sane sub-bands ───────────────
    for (int i = 0; i < REGION_COUNTRY_COUNT; i++) {
        const regulatory_country_t *c = region_get_country_by_index(i);
        EXPECT(c && c->country_code && c->display_name,
               "country %d has code + name", i);
        if (!c || !c->subbands) continue;  // sentinel
        EXPECT(c->n_subbands > 0, "%s has sub-bands", c->country_code);
        for (uint8_t b = 0; b < c->n_subbands; b++) {
            const regulatory_subband_t *sb = &c->subbands[b];
            EXPECT(sb->freq_min_mhz <= sb->freq_max_mhz,
                   "%s/%s freq_min ≤ freq_max", c->country_code, sb->label);
            EXPECT(sb->duty_cycle_permille >= 1 && sb->duty_cycle_permille <= 1000,
                   "%s/%s duty in 1..1000", c->country_code, sb->label);
        }
    }

    if (failures == 0) {
        printf("OK -- all region-limits test vectors passed\n");
        return 0;
    }
    fprintf(stderr, "%d test vector failure(s)\n", failures);
    return 1;
}
