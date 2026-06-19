// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
// SPDX-FileContributor: Ilias el Matani <hello@ilias.codes>

#include "gps_parser.h"

#include <stdlib.h>
#include <string.h>

bool gps_nmea_checksum_ok(const char *sentence) {
    if (!sentence || sentence[0] != '$') return false;
    unsigned char sum = 0;
    const char   *p   = sentence + 1;
    while (*p && *p != '*') {
        sum ^= (unsigned char)*p;
        p++;
    }
    if (*p != '*' || !p[1] || !p[2]) return false;
    char     hex[3] = { p[1], p[2], 0 };
    unsigned long expect = strtoul(hex, NULL, 16);
    return (unsigned char)expect == sum;
}

// Split on ',' KEEPING empty fields (NMEA uses ",," for absent values, so
// strtok is unusable here). Stops at '*' so the checksum suffix is dropped.
// Field pointers index into buf, which is modified in place.
static int split_fields_keep_empty(char *buf, char *fields[], int max_fields) {
    int n = 0;
    if (max_fields <= 0) return 0;
    fields[n++] = buf;
    for (char *p = buf; *p; p++) {
        if (*p == '*') { *p = '\0'; break; }
        if (*p == ',') {
            *p = '\0';
            if (n < max_fields) fields[n++] = p + 1;
        }
    }
    return n;
}

// "ddmm.mmmm" (+ hemisphere char) → microdegrees. Works for both lat
// (ddmm.mmmm) and lon (dddmm.mmmm): minutes are always two digits left of
// the dot, so dividing by 100 isolates whole degrees regardless of how
// many degree-digits precede them.
static bool dmm_to_e6(const char *field, char hemi, int32_t *out_e6) {
    if (!field || !*field) return false;
    char *end;
    double dmm = strtod(field, &end);
    if (end == field) return false;
    int    deg = (int)(dmm / 100.0);
    double min = dmm - (deg * 100.0);
    double dd  = deg + (min / 60.0);
    if (hemi == 'S' || hemi == 'W') dd = -dd;
    *out_e6 = (int32_t)(dd * 1e6);
    return true;
}

bool gps_nmea_apply_line(const char *line, gps_status_t *st) {
    if (!line || !st || line[0] != '$') return false;

    // Work on a local copy: split_fields_keep_empty is destructive, and we
    // also need to strip CR/LF defensively so callers that pass in a raw
    // line still work.
    char buf[128];
    size_t n = 0;
    while (n < sizeof(buf) - 1 && line[n] && line[n] != '\r' && line[n] != '\n') {
        buf[n] = line[n];
        n++;
    }
    buf[n] = '\0';

    if (!gps_nmea_checksum_ok(buf)) return false;

    char *f[20] = {0};
    int   nf   = split_fields_keep_empty(buf, f, 20);
    if (nf < 1) return false;

    // Match the 3-char sentence id, talker-agnostic ($GNRMC, $GPRMC, $GLRMC
    // all resolve to "RMC"). The old `line + 3` form silently breaks on any
    // talker longer than two chars; this also makes future $GAxxx / $GBxxx
    // (Galileo / BeiDou) sentences "just work".
    size_t      tlen = strlen(f[0]);
    const char *type = (tlen >= 3) ? f[0] + tlen - 3 : f[0];

    if (strcmp(type, "RMC") == 0 && nf >= 7) {
        // 1:time 2:status 3:lat 4:N/S 5:lon 6:E/W
        if (f[2][0] == 'A') {
            int32_t lat_e6, lon_e6;
            if (dmm_to_e6(f[3], f[4][0], &lat_e6) &&
                dmm_to_e6(f[5], f[6][0], &lon_e6)) {
                st->lat_e6    = lat_e6;
                st->lon_e6    = lon_e6;
                st->fix_valid = true;
            }
        }
        return true;
    }

    if (strcmp(type, "GGA") == 0 && nf >= 9) {
        // 1:time 2:lat 3:N/S 4:lon 5:E/W 6:quality 7:numSats 8:HDOP
        st->fix_quality   = atoi(f[6]);
        st->fix_used_sats = atoi(f[7]);
        st->hdop          = (float)atof(f[8]);
        if (st->fix_quality > 0 && f[2][0]) {
            int32_t lat_e6, lon_e6;
            if (dmm_to_e6(f[2], f[3][0], &lat_e6) &&
                dmm_to_e6(f[4], f[5][0], &lon_e6)) {
                st->lat_e6    = lat_e6;
                st->lon_e6    = lon_e6;
                st->fix_valid = true;
            }
        }
        return true;
    }

    if (strcmp(type, "GSV") == 0 && nf >= 4) {
        // Field 3 is total satellites in view across all GSV messages of this
        // set. Talker ID picks which constellation: GP=GPS, GL=GLONASS.
        int sats = atoi(f[3]);
        if (tlen >= 5 && f[0][1] == 'G' && f[0][2] == 'P') st->gps_sats_view = sats;
        else if (tlen >= 5 && f[0][1] == 'G' && f[0][2] == 'L') st->glo_sats_view = sats;
        return true;
    }

    return false;
}
