// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// One-shot NMEA reader for the Adafruit Mini GPS PA1010D (#4415) attached
// to the Tanmatsu QWIIC connector (ESP32-P4 I2C port 1, GPIO33/SDA, GPIO32/SCL).

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool    bus_ok;          // false if QWIIC bus init or 0x10 probe failed
    int     sentences_seen;  // total NMEA sentences successfully parsed
    int     gps_sats_view;   // satellites visible (latest $GPGSV)
    int     glo_sats_view;   // GLONASS satellites visible (latest $GLGSV)
    int     fix_used_sats;   // sats used in current fix (latest $GNGGA field 7)
    int     fix_quality;     // 0=no fix, 1=GPS, 2=DGPS (latest $GNGGA field 6)
    float   hdop;            // horizontal dilution of precision (latest $GNGGA field 8)
    bool    fix_valid;       // true iff a fix was obtained (lat_e6/lon_e6 valid)
    int32_t lat_e6;          // 1e-6 degrees, valid if fix_valid
    int32_t lon_e6;          // 1e-6 degrees, valid if fix_valid
} gps_status_t;

// Read NMEA from the QWIIC-attached PA1010D for up to timeout_ms or until a
// valid fix is obtained. Returns true if any NMEA sentences were parsed at
// all (regardless of fix). status->fix_valid tells whether lat/lon are usable.
// Caller is expected to show a "Searching..." indicator — this call blocks.
bool gps_read_status(int timeout_ms, gps_status_t* out);

// Copy the most recent gps_read_status() result into `out`. Returns false if
// no scan has run this session (or every scan returned zero sentences). Used
// by the Settings UI to keep the sats/HDOP info line after the toast fades.
bool gps_last_status(gps_status_t* out);
