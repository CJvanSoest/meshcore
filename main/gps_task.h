// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// Background GPS task. Polls the PA1010D on the QWIIC bus at a cadence
// chosen by the user-selected transport profile (Walking / Cycling /
// Driving / Manual), and publishes the latest fix under a short critical
// section so VIEW_MAP, the status strip, and any future companion-protocol
// push can read it without blocking. Manual profile = the task is idle;
// the one-shot Auto-fill action in Settings still works as before.

#pragma once

#include <stdbool.h>
#include <stdint.h>

// Transport profiles drive both the poll interval (how often the chip is
// read) and the commit distance threshold (how far we must have moved to
// publish + log a new position). Heartbeat caps below guarantee SAT / HDOP
// still refresh even when the user is stationary.
typedef enum {
    GPS_PROFILE_WALKING = 0,
    GPS_PROFILE_CYCLING = 1,
    GPS_PROFILE_DRIVING = 2,
    GPS_PROFILE_MANUAL  = 3,
    GPS_PROFILE_COUNT,
} gps_profile_t;

// User-tunable runtime state. The Settings UI binds these to NVS rows;
// gps_task reads them every tick so changes take effect on the next poll.
extern gps_profile_t gps_profile;
extern uint16_t      gps_custom_interval_s;   // 0 = use profile default
extern uint16_t      gps_custom_distance_m;   // 0 = use profile default

// Published fix state. Readers may grab the values directly under the
// `gps_live_lock` critical section; writes happen only inside gps_task.
extern int32_t  gps_live_lat_e6;
extern int32_t  gps_live_lon_e6;
extern uint8_t  gps_live_sats;          // gps_sats_view + glo_sats_view
extern float    gps_live_hdop;
extern uint32_t gps_live_last_fix_ms;   // 0 = never; xTaskGetTickCount × portTICK_PERIOD_MS
extern bool     gps_live_valid;
extern bool     gps_live_bus_ok;        // true after first successful bus probe

// Spawn the task. Idempotent; safe to call multiple times.
void gps_task_start(void);

// Friendly label for a profile (used by the Settings fmt_field cases).
const char *gps_profile_label(gps_profile_t p);
