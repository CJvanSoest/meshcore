// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
// SPDX-FileContributor: Ilias el Matani <hello@ilias.codes>
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

// gps_profile_t + the user-tunable gps_profile / gps_custom_* globals and the
// gps_profile_label helper live in the neutral config_types.h so the L1
// settings store can read them without depending on this task module.
#include "config_types.h"

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
