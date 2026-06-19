// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
// SPDX-FileContributor: Ilias el Matani <hello@ilias.codes>
//
// Edge-case host test for the NMEA-0183 parser, complementing test_gps_nmea.c.
// Where test_gps_nmea.c proves the happy path (valid GGA/RMC/GSV, checksum
// gate, talker-agnostic match), this file walks the branches that the happy
// path never reaches:
//
//   - gps_nmea_checksum_ok rejection paths (NULL, no '$', truncated suffix)
//   - NULL / non-'$' guards in gps_nmea_apply_line
//   - RMC status field 'V' (void) leaves fix_valid false
//   - GGA fix_quality 0 records sats/HDOP but writes no coordinates
//   - southern / western hemisphere produces negative microdegrees
//   - boundary lat/lon (equator/prime-meridian, near-pole, near-antimeridian)
//   - unknown sentence type and short field counts are ignored
//
// Links the SAME gps_parser.c that ships in firmware.
//
// Build (mirrors tests/Makefile test_gps_parser_edge target):
//     gcc -I../components/mc_proto test_gps_parser_edge.c
//         ../components/mc_proto/gps_parser.c -o test_gps_parser_edge -lm
//
// Exit 0 on pass, 1 on any vector mismatch.

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gps.h"
#include "gps_parser.h"

static int failures = 0;

#define EXPECT(cond, fmt, ...) do {                                            \
    if (!(cond)) {                                                             \
        fprintf(stderr, "FAIL %s:%d: " fmt "\n", __FILE__, __LINE__,          \
                ##__VA_ARGS__);                                               \
        failures++;                                                           \
    }                                                                         \
} while (0)

// Build a full '$'-prefixed sentence with a correct XOR checksum from a body
// (body has no '$', no '*XX'). Same helper shape as test_gps_nmea.c so the
// vectors below read the same way.
static void make_sentence(char *out, size_t out_sz, const char *body) {
    unsigned char sum = 0;
    for (const char *p = body; *p; p++) sum ^= (unsigned char)*p;
    snprintf(out, out_sz, "$%s*%02X", body, sum);
}

static bool feed(gps_status_t *st, const char *body) {
    char line[160];
    make_sentence(line, sizeof(line), body);
    return gps_nmea_apply_line(line, st);
}

int main(void) {
    gps_status_t st;

    // ── checksum_ok rejection paths (the function the parser gates on) ──
    EXPECT(!gps_nmea_checksum_ok(NULL),        "NULL must be rejected");
    EXPECT(!gps_nmea_checksum_ok(""),          "empty string must be rejected");
    EXPECT(!gps_nmea_checksum_ok("GPRMC,A*00"), "missing '$' must be rejected");
    // '*' present but only one hex digit after it: p[2] is the NUL terminator.
    EXPECT(!gps_nmea_checksum_ok("$GPGGA*0"),  "truncated checksum suffix must be rejected");
    // '$' immediately followed by '*' then two hex digits: sum over zero bytes
    // is 0x00, so "*00" is the only checksum that validates here.
    EXPECT(gps_nmea_checksum_ok("$*00"),       "empty body with matching 00 checksum is valid");
    EXPECT(!gps_nmea_checksum_ok("$*01"),      "empty body with non-zero checksum is invalid");

    // ── apply_line NULL / non-'$' guards ──
    memset(&st, 0, sizeof(st));
    EXPECT(!gps_nmea_apply_line(NULL, &st),    "NULL line must be rejected");
    EXPECT(!gps_nmea_apply_line("$GPGGA*00", NULL), "NULL status must be rejected");
    EXPECT(!gps_nmea_apply_line("GPRMC,no dollar", &st), "non-'$' line must be rejected");

    // ── RMC status 'V' (void / no fix): recognised sentence (returns true)
    //    but must NOT set fix_valid or write coordinates. ──
    memset(&st, 0, sizeof(st));
    bool rmc_void = feed(&st,
        "GNRMC,140555.000,V,5152.3082,N,00517.4878,E,0.31,54.45,080625,,,N");
    EXPECT(rmc_void,        "void RMC is still a recognised sentence");
    EXPECT(!st.fix_valid,   "void RMC must leave fix_valid false");
    EXPECT(st.lat_e6 == 0,  "void RMC must not write lat_e6, got %ld", (long)st.lat_e6);

    // ── GGA fix_quality 0: no fix, but the sats/HDOP fields are still read.
    //    fix_valid must stay false and no coordinate written. ──
    memset(&st, 0, sizeof(st));
    bool gga_nofix = feed(&st,
        "GNGGA,140556.000,,,,,0,00,99.99,,M,,M,,");
    EXPECT(gga_nofix,            "quality-0 GGA is still recognised");
    EXPECT(st.fix_quality == 0,  "quality 0 recorded, got %d", st.fix_quality);
    EXPECT(st.fix_used_sats == 0, "0 sats recorded, got %d", st.fix_used_sats);
    EXPECT(fabsf(st.hdop - 99.99f) < 0.01f, "hdop 99.99 recorded, got %.3f", st.hdop);
    EXPECT(!st.fix_valid,        "quality-0 GGA must leave fix_valid false");
    EXPECT(st.lat_e6 == 0,       "quality-0 GGA must not write lat_e6");

    // ── Southern + western hemisphere: both coordinates negative. Sydney
    //    33.8688 S, 151.2093 E -> negative lat, positive lon. Then a second
    //    vector with W longitude to hit the 'W' branch of dmm_to_e6. ──
    memset(&st, 0, sizeof(st));
    // 3352.128 S = -(33 + 52.128/60), 15112.558 E = 151 + 12.558/60.
    feed(&st, "GNRMC,000000.000,A,3352.1280,S,15112.5580,E,0,0,010120,,,A");
    EXPECT(st.fix_valid, "southern-hemisphere RMC must produce a fix");
    EXPECT(st.lat_e6 < 0, "S latitude must be negative, got %ld", (long)st.lat_e6);
    EXPECT(st.lon_e6 > 0, "E longitude must be positive, got %ld", (long)st.lon_e6);
    {
        double exp_lat = -(33.0 + 52.128 / 60.0);
        int32_t exp_e6 = (int32_t)(exp_lat * 1e6);
        EXPECT(abs(st.lat_e6 - exp_e6) <= 2,
               "S lat_e6 expected ~%d, got %ld", exp_e6, (long)st.lat_e6);
    }

    memset(&st, 0, sizeof(st));
    // San Francisco 37.7749 N, 122.4194 W. 3746.494 N, 12225.164 W.
    feed(&st, "GNRMC,000000.000,A,3746.4940,N,12225.1640,W,0,0,010120,,,A");
    EXPECT(st.fix_valid, "western-hemisphere RMC must produce a fix");
    EXPECT(st.lon_e6 < 0, "W longitude must be negative, got %ld", (long)st.lon_e6);
    {
        double exp_lon = -(122.0 + 25.164 / 60.0);
        int32_t exp_e6 = (int32_t)(exp_lon * 1e6);
        EXPECT(abs(st.lon_e6 - exp_e6) <= 2,
               "W lon_e6 expected ~%d, got %ld", exp_e6, (long)st.lon_e6);
    }

    // ── Boundary coordinates: equator + prime meridian collapse to 0. ──
    memset(&st, 0, sizeof(st));
    feed(&st, "GNRMC,000000.000,A,0000.0000,N,00000.0000,E,0,0,010120,,,A");
    EXPECT(st.fix_valid,    "0,0 fix is valid");
    EXPECT(st.lat_e6 == 0,  "equator lat_e6 must be 0, got %ld", (long)st.lat_e6);
    EXPECT(st.lon_e6 == 0,  "prime-meridian lon_e6 must be 0, got %ld", (long)st.lon_e6);

    // ── Near-pole / near-antimeridian: largest magnitudes the wire can carry,
    //    must stay inside int32 microdegrees (±180e6 fits in int32). ──
    memset(&st, 0, sizeof(st));
    // 89 deg 54' N = 89.9, 179 deg 54' W = -179.9.
    feed(&st, "GNRMC,000000.000,A,8954.0000,N,17954.0000,W,0,0,010120,,,A");
    EXPECT(st.fix_valid, "near-pole fix must be valid");
    {
        int32_t exp_lat = (int32_t)((89.0 + 54.0 / 60.0) * 1e6);
        int32_t exp_lon = (int32_t)(-(179.0 + 54.0 / 60.0) * 1e6);
        EXPECT(abs(st.lat_e6 - exp_lat) <= 2,
               "near-pole lat_e6 expected ~%d, got %ld", exp_lat, (long)st.lat_e6);
        EXPECT(abs(st.lon_e6 - exp_lon) <= 2,
               "near-antimeridian lon_e6 expected ~%d, got %ld", exp_lon, (long)st.lon_e6);
        EXPECT(st.lon_e6 > -180000000, "lon_e6 must stay above -180e6");
    }

    // ── Unknown sentence type: valid checksum, recognised talker, but VTG is
    //    not handled -> apply_line returns false and writes nothing. ──
    memset(&st, 0, sizeof(st));
    bool vtg = feed(&st, "GNVTG,054.7,T,034.4,M,005.5,N,010.2,K,A");
    EXPECT(!vtg,           "unhandled VTG sentence must return false");
    EXPECT(!st.fix_valid,  "VTG must not touch fix_valid");

    // ── Short field counts: each handler guards on a minimum nf. A truncated
    //    RMC/GGA/GSV must be dropped (return false) rather than read past the
    //    fields that are present. ──
    memset(&st, 0, sizeof(st));
    EXPECT(!feed(&st, "GNRMC,140555.000,A,5152.3082"),
           "short RMC (nf<7) must be ignored");
    EXPECT(!feed(&st, "GNGGA,140556.000,5152.3083,N,00517.4878"),
           "short GGA (nf<9) must be ignored");
    EXPECT(!feed(&st, "GPGSV,1,1"),
           "short GSV (nf<4) must be ignored");
    EXPECT(!st.fix_valid, "no short sentence may set fix_valid");

    // ── GSV with a 4-char talker (e.g. a bare "GSV" id) must not index past
    //    f[0][2]; the tlen>=5 guard protects this. A GAGSV (Galileo) is a
    //    recognised GSV sentence but not bucketed into GPS/GLONASS counters. ──
    memset(&st, 0, sizeof(st));
    bool ga_gsv = feed(&st, "GAGSV,1,1,03,01,,,20,02,,,22,03,,,19");
    EXPECT(ga_gsv,                "Galileo GSV is a recognised sentence");
    EXPECT(st.gps_sats_view == 0, "Galileo GSV must not fill the GPS bucket");
    EXPECT(st.glo_sats_view == 0, "Galileo GSV must not fill the GLONASS bucket");

    // ── Coordinate stickiness: a fresh valid GGA fix overwrites a prior RMC
    //    fix; the void-RMC path above already proved a non-fix never clobbers
    //    valid coordinates within the same scan. Re-feeding a good RMC after a
    //    void one must restore the fix. ──
    memset(&st, 0, sizeof(st));
    feed(&st, "GNRMC,000000.000,V,0000.0000,N,00000.0000,E,0,0,010120,,,N");
    EXPECT(!st.fix_valid, "void RMC leaves no fix");
    feed(&st, "GNRMC,000001.000,A,5152.3082,N,00517.4878,E,0,0,010120,,,A");
    EXPECT(st.fix_valid, "subsequent valid RMC restores the fix");
    EXPECT(st.lat_e6 > 0, "restored fix has a positive northern latitude");

    if (failures == 0) {
        printf("OK -- all NMEA edge-case vectors passed\n");
        return 0;
    }
    fprintf(stderr, "%d test vector failure(s)\n", failures);
    return 1;
}
