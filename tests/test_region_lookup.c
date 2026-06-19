// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
// SPDX-FileContributor: Ilias el Matani <hello@ilias.codes>
//
// Host-side test for the regulatory lookup + helper API in
// components/mc_proto/region_limits.c.
//
// test_region_limits.c already guards the sub-band math; this file focuses on
// the public lookup/helper surface across the whole country table: ISO lookup
// (valid + unknown), index bounds, in-band vs off-band frequency matching, the
// ERP/EIRP power conversion with its 2.15 dBi dipole offset, and the duty-cycle
// budget. Vectors use real values from the shipping table (JP/KR strict bands,
// RU dual windows, ANZ, IN) so a flipped power unit or a shifted band boundary
// makes CI red before it can merge.
//
// Links against the SAME translation unit the firmware ships.
//
// Build (see tests/Makefile):
//     gcc -I../components/mc_proto test_region_lookup.c
//         ../components/mc_proto/region_limits.c -o test_region_lookup
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

static const char *band_at(const regulatory_country_t *c, float f) {
    const regulatory_subband_t *sb = region_match_subband(c, f);
    return sb ? sb->label : "(none)";
}

int main(void) {
    // ── region_get_country: valid lookups across power units ─────────────────
    const regulatory_country_t *de = region_get_country("DE");
    EXPECT(de != NULL, "DE must resolve");
    EXPECT(de && strcmp(de->display_name, "Germany") == 0, "DE display_name");
    EXPECT(de && de->power_unit == POWER_UNIT_ERP, "DE regulates in ERP");

    const regulatory_country_t *jp = region_get_country("JP");
    EXPECT(jp && strcmp(jp->display_name, "Japan") == 0, "JP display_name");
    EXPECT(jp && jp->power_unit == POWER_UNIT_EIRP, "JP regulates in EIRP");
    EXPECT(jp && jp->n_subbands == 1, "JP has 1 sub-band");

    // Case-insensitive on a mixed-case and multi-char code.
    EXPECT(region_get_country("jp") == jp, "lower-case jp resolves to JP");
    const regulatory_country_t *eu433 = region_get_country("eU-433");
    EXPECT(eu433 && strcmp(eu433->display_name, "EU 433 MHz") == 0,
           "mixed-case eU-433 resolves");

    // Distinct EU countries share the same sub-band table pointer (harmonised).
    const regulatory_country_t *be = region_get_country("BE");
    EXPECT(be && be->subbands == de->subbands,
           "EU868 countries share one sub-band table");

    // ── region_get_country: unknown / malformed ISO codes → NULL ─────────────
    EXPECT(region_get_country("XX") == NULL, "unknown XX → NULL");
    EXPECT(region_get_country("USA") == NULL, "over-long USA → NULL (no prefix match)");
    EXPECT(region_get_country("U") == NULL, "partial U → NULL (no prefix match)");
    EXPECT(region_get_country(NULL) == NULL, "NULL code → NULL");
    EXPECT(region_get_country("") == NULL, "empty code → NULL");

    // ── region_get_country_by_index: bounds + round-trip ─────────────────────
    EXPECT(region_get_country_by_index(0) != NULL, "index 0 valid");
    EXPECT(region_get_country_by_index(REGION_COUNTRY_COUNT - 1) != NULL,
           "last index valid");
    EXPECT(region_get_country_by_index(REGION_COUNTRY_COUNT) == NULL,
           "one past the end → NULL");
    EXPECT(region_get_country_by_index(-1) == NULL, "negative index → NULL");
    EXPECT(region_get_country_by_index(-1000) == NULL, "far-negative index → NULL");

    // Index lookup and ISO lookup agree for a known entry.
    const regulatory_country_t *jp_by_iso = region_get_country("JP");
    int jp_idx = -1;
    for (int i = 0; i < REGION_COUNTRY_COUNT; i++) {
        if (region_get_country_by_index(i) == jp_by_iso) { jp_idx = i; break; }
    }
    EXPECT(jp_idx >= 0, "JP found by index scan");
    EXPECT(jp_idx >= 0 && region_get_country_by_index(jp_idx) == jp_by_iso,
           "index lookup round-trips to the same JP pointer");

    // ── region_match_subband: in-band vs off-band ────────────────────────────
    // JP 920.500-923.500: edges inclusive, just outside excluded.
    EXPECT(strcmp(band_at(jp, 922.0f), "JP920") == 0, "JP 922.0 in band");
    EXPECT(region_match_subband(jp, 920.5f) != NULL, "JP lower edge inclusive");
    EXPECT(region_match_subband(jp, 923.5f) != NULL, "JP upper edge inclusive");
    EXPECT(region_match_subband(jp, 920.499f) == NULL, "just below JP band → NULL");
    EXPECT(region_match_subband(jp, 923.501f) == NULL, "just above JP band → NULL");
    EXPECT(jp && jp->subbands[0].lbt_alternative == true,
           "JP band carries LBT-required flag");

    // RU has two non-overlapping windows; the gap between them is off-band.
    const regulatory_country_t *ru = region_get_country("RU");
    EXPECT(ru && ru->n_subbands == 2, "RU has 2 sub-bands");
    EXPECT(strcmp(band_at(ru, 864.5f), "RU864") == 0, "RU 864.5 → RU864");
    EXPECT(strcmp(band_at(ru, 869.0f), "RU869") == 0, "RU 869.0 → RU869");
    EXPECT(region_match_subband(ru, 867.0f) == NULL,
           "RU 867.0 falls in the inter-window gap");

    // ANZ and IN single bands.
    const regulatory_country_t *au = region_get_country("AU");
    EXPECT(strcmp(band_at(au, 920.0f), "ANZ915") == 0, "AU 920.0 → ANZ915");
    EXPECT(region_match_subband(au, 902.0f) == NULL,
           "AU 902.0 below ANZ915 lower edge → NULL");
    const regulatory_country_t *in = region_get_country("IN");
    EXPECT(strcmp(band_at(in, 866.0f), "IN865") == 0, "IN 866.0 → IN865");

    // NULL country / NULL sub-band table are safe.
    EXPECT(region_match_subband(NULL, 868.0f) == NULL, "NULL country → NULL band");

    // ── region_effective_power_dbm: ERP/EIRP + 2.15 dBi offset ───────────────
    // ERP subtracts the dipole reference (2.15 dBi, rounded down to 2).
    EXPECT(region_effective_power_dbm(de, 20, 0) == 18, "ERP: 20+0-2 = 18");
    EXPECT(region_effective_power_dbm(de, 20, 5) == 23, "ERP: 20+5-2 = 23");
    EXPECT(region_effective_power_dbm(de, 0, 0) == -2, "ERP: 0+0-2 = -2");
    // EIRP keeps conducted + gain with no dipole correction.
    EXPECT(region_effective_power_dbm(jp, 13, 0) == 13, "EIRP: 13+0 = 13");
    EXPECT(region_effective_power_dbm(au, 30, 0) == 30, "EIRP: 30+0 = 30");
    EXPECT(region_effective_power_dbm(au, 20, 3) == 23, "EIRP: 20+3 = 23");
    // Negative antenna gain (loss) is honoured.
    EXPECT(region_effective_power_dbm(au, 10, -4) == 6, "EIRP: 10-4 = 6");
    EXPECT(region_effective_power_dbm(de, 10, -4) == 4, "ERP: 10-4-2 = 4");
    // NULL country returns conducted power unchanged.
    EXPECT(region_effective_power_dbm(NULL, 27, 9) == 27,
           "NULL country → conducted power unchanged");
    // Upper clamp at +127 (int8_t saturation).
    EXPECT(region_effective_power_dbm(au, 127, 100) == 127, "EIRP clamps at +127");
    // Lower clamp at -128: conducted + gain - 2 underflows the int8_t.
    EXPECT(region_effective_power_dbm(de, -120, -20) == -128, "ERP clamps at the int8 floor");

    // ── region_dc_budget_ms_per_hour: permille × 3600 ────────────────────────
    // JP/KR/RU bands use 100 permille (10%) and 1 permille (0.1%).
    const regulatory_subband_t *jp920 = region_match_subband(jp, 922.0f);
    EXPECT(region_dc_budget_ms_per_hour(jp920) == 360000u, "JP 10%% → 360000 ms/h");
    const regulatory_subband_t *ru864 = region_match_subband(ru, 864.5f);
    EXPECT(region_dc_budget_ms_per_hour(ru864) == 3600u, "RU 0.1%% → 3600 ms/h");
    const regulatory_subband_t *anz = region_match_subband(au, 920.0f);
    EXPECT(region_dc_budget_ms_per_hour(anz) == 3600000u, "ANZ no limit → 3600000 ms/h");
    EXPECT(region_dc_budget_ms_per_hour(NULL) == 0u, "NULL sub-band → 0 budget");

    if (failures == 0) {
        printf("OK -- all region-lookup test vectors passed\n");
        return 0;
    }
    fprintf(stderr, "%d test vector failure(s)\n", failures);
    return 1;
}
