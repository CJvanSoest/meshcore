// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
// SPDX-FileContributor: Ilias el Matani <hello@ilias.codes>
//
// Shared config enums plus their runtime globals, in a neutral low header so
// settings_nvs (the L1 config store) can read and write them without
// depending on the gps_task or map modules that own the storage. The enum
// values, the variable definitions and the label implementations stay with
// their owning modules; only the types, the externs and the pure label
// helpers are declared here. This is what lets settings_nvs drop its upward
// includes of gps_task.h / map.h.

#pragma once

#include <stdbool.h>
#include <stdint.h>

// GPS transport profile (owner: gps_task.c).
typedef enum {
    GPS_PROFILE_WALKING = 0,
    GPS_PROFILE_CYCLING = 1,
    GPS_PROFILE_DRIVING = 2,
    GPS_PROFILE_MANUAL  = 3,
    GPS_PROFILE_COUNT,
} gps_profile_t;

extern gps_profile_t gps_profile;
extern uint16_t      gps_custom_interval_s;   // 0 = use profile default
extern uint16_t      gps_custom_distance_m;   // 0 = use profile default

const char *gps_profile_label(gps_profile_t p);

// Map style profile (owner: map.c).
typedef enum {
    MAP_PROFILE_RIPPLE = 0,
    MAP_PROFILE_CARTO  = 1,
    MAP_PROFILE_CYCLE  = 2,
    MAP_PROFILE_TOPO   = 3,
    MAP_PROFILE_COUNT,
} map_profile_t;

extern map_profile_t map_profile;
extern bool          map_lock_on;

const char *map_profile_label(map_profile_t p);
