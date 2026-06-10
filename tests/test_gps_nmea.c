// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// Host-side test for the NMEA-0183 parser shared between firmware and tests.
//
// Links against main/gps_parser.c -- the SAME translation unit that ships in
// the device firmware. Catches parser regressions before merge so a broken
// sentence handler can never reach the release pipeline.
//
// Build (see tests/Makefile):
//     gcc -I../main test_gps_nmea.c ../main/gps_parser.c -o test_gps_nmea
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
        fprintf(stderr, "FAIL %s:%d: " fmt "\n", __FILE__, __LINE__,           \
                ##__VA_ARGS__);                                                \
        failures++;                                                            \
    }                                                                          \
} while (0)

// Build a full sentence with computed checksum from a body (no '$', no '*XX').
// We exercise the real checksum path -- the parser must reject sentences with
// an incorrect or missing checksum.
static void feed_body(gps_status_t *st, const char *body) {
    char          line[128];
    unsigned char sum = 0;
    for (const char *p = body; *p; p++) sum ^= (unsigned char)*p;
    snprintf(line, sizeof(line), "$%s*%02X", body, sum);
    bool parsed = gps_nmea_apply_line(line, st);
    if (!parsed) {
        fprintf(stderr, "NOTE: parser ignored sentence: %s\n", line);
    }
}

int main(void) {
    // ── TV1: real fix captured off the Tanmatsu GPS-test screen (51.87° N,
    //         5.29° E -- somewhere in NL). GGA then RMC, same coords, same
    //         time. Final state must reflect both: GGA contributes
    //         fix_quality / num_sats / HDOP, RMC overwrites lat/lon last.
    gps_status_t st;
    memset(&st, 0, sizeof(st));

    feed_body(&st, "GNGGA,140556.000,5152.3083,N,00517.4878,E,1,05,2.47,12.0,M,47.2,M,,");
    feed_body(&st, "GNRMC,140555.000,A,5152.3082,N,00517.4878,E,0.31,54.45,080625,,,A");

    // Expected fix coords -- compute in test rather than hard-coding so the
    // tolerance below is the only fudge factor. 5152.3082 → 51 + 52.3082/60.
    double exp_lat = 51.0 + 52.3082 / 60.0;
    double exp_lon =  5.0 + 17.4878 / 60.0;
    int32_t exp_lat_e6 = (int32_t)(exp_lat * 1e6);
    int32_t exp_lon_e6 = (int32_t)(exp_lon * 1e6);

    EXPECT(st.fix_valid,          "fix_valid expected true");
    EXPECT(st.fix_quality == 1,   "fix_quality expected 1, got %d", st.fix_quality);
    EXPECT(st.fix_used_sats == 5, "fix_used_sats expected 5, got %d", st.fix_used_sats);
    EXPECT(fabsf(st.hdop - 2.47f) < 0.01f, "hdop expected 2.47, got %.3f", st.hdop);
    EXPECT(abs(st.lat_e6 - exp_lat_e6) <= 2,
           "lat_e6 expected ~%d, got %ld", exp_lat_e6, (long)st.lat_e6);
    EXPECT(abs(st.lon_e6 - exp_lon_e6) <= 2,
           "lon_e6 expected ~%d, got %ld", exp_lon_e6, (long)st.lon_e6);

    // ── TV2: GSV updates the satellites-in-view counters per constellation.
    //         Field 3 = total visible. The talker prefix picks the bucket.
    memset(&st, 0, sizeof(st));
    feed_body(&st, "GPGSV,1,1,07,01,,,28,02,,,30,03,,,29,04,,,31");  // 7 GPS sats
    feed_body(&st, "GLGSV,1,1,04,65,,,33,66,,,30,67,,,28,68,,,26"); // 4 GLO sats
    EXPECT(st.gps_sats_view == 7, "gps_sats_view expected 7, got %d", st.gps_sats_view);
    EXPECT(st.glo_sats_view == 4, "glo_sats_view expected 4, got %d", st.glo_sats_view);

    // ── TV3: checksum gate. Same RMC body, but corrupt one digit AFTER the
    //         '*' (i.e. flip a checksum bit). Parser must reject and leave
    //         state untouched.
    memset(&st, 0, sizeof(st));
    bool parsed_bad = gps_nmea_apply_line(
        "$GNRMC,140555.000,A,5152.3082,N,00517.4878,E,0.31,54.45,080625,,,A*00",
        &st);
    EXPECT(!parsed_bad,    "bad-checksum sentence must be rejected");
    EXPECT(!st.fix_valid,  "bad-checksum sentence must not set fix_valid");
    EXPECT(st.lat_e6 == 0, "bad-checksum sentence must not write lat_e6");

    // ── TV4: missing checksum suffix. Must reject too.
    memset(&st, 0, sizeof(st));
    bool parsed_no_cs = gps_nmea_apply_line(
        "$GNRMC,140555.000,A,5152.3082,N,00517.4878,E,0.31,54.45,080625,,,A",
        &st);
    EXPECT(!parsed_no_cs, "checksumless sentence must be rejected");

    // ── TV5: talker-agnostic match. $GPRMC and $GLRMC produce the same
    //         result as $GNRMC for identical bodies.
    memset(&st, 0, sizeof(st));
    feed_body(&st, "GPRMC,140555.000,A,5152.3082,N,00517.4878,E,0.31,54.45,080625,,,A");
    EXPECT(st.fix_valid, "GPRMC must be parsed like GNRMC (talker-agnostic)");

    if (failures == 0) {
        printf("OK -- all NMEA test vectors passed\n");
        return 0;
    }
    fprintf(stderr, "%d test vector failure(s)\n", failures);
    return 1;
}
