// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// Pure-C NMEA-0183 parser for the PA1010D and any GPS that streams standard
// $GxRMC / $GxGGA / $GxGSV sentences. No malloc, no IDF deps, no FreeRTOS --
// safe to link from host_tests/. The I2C transport lives in gps.c; this file
// only knows about lines of ASCII.

#pragma once

#include <stdbool.h>
#include "gps.h"

// Verify the NMEA XOR checksum: XOR of every byte between '$' and '*' must
// equal the two hex digits after '*'. Returns false on any malformed input.
// Defensive: a corrupt I2C frame produces garbage fields, so we drop it.
bool gps_nmea_checksum_ok(const char* sentence);

// Parse one NMEA sentence (no CR/LF, '$'-prefixed) and update `st` in place.
// Returns true if the sentence had a valid checksum AND was a type we
// understood (RMC / GGA / GSV). The talker prefix is matched generically:
// $GNRMC, $GPRMC, $GLRMC all resolve to RMC, so multi-constellation modules
// work out of the box.
bool gps_nmea_apply_line(const char* line, gps_status_t* st);
