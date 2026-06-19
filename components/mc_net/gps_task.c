// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "gps_task.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "gps.h"

static const char *TAG = "gps_task";

// Hard ceiling so a typo'd custom interval (e.g. someone enters 0xFFFF
// seconds) doesn't put the chip to sleep for half a day.
#define GPS_HEARTBEAT_MAX_S    30

// Profile-default poll intervals (seconds between PA1010D reads).
static const uint16_t s_profile_interval_s[GPS_PROFILE_COUNT] = {
    [GPS_PROFILE_WALKING] = 10,
    [GPS_PROFILE_CYCLING] = 5,
    [GPS_PROFILE_DRIVING] = 2,
    [GPS_PROFILE_MANUAL]  = 0,  // 0 = task idles
};
// Profile-default commit distances (metres; 0 = commit every poll).
static const uint16_t s_profile_distance_m[GPS_PROFILE_COUNT] = {
    [GPS_PROFILE_WALKING] = 5,
    [GPS_PROFILE_CYCLING] = 25,
    [GPS_PROFILE_DRIVING] = 50,
    [GPS_PROFILE_MANUAL]  = 0,
};

// ── User-tunable runtime state ──────────────────────────────────────────────
gps_profile_t gps_profile           = GPS_PROFILE_WALKING;
uint16_t      gps_custom_interval_s = 0;
uint16_t      gps_custom_distance_m = 0;

// ── Published fix state ─────────────────────────────────────────────────────
int32_t  gps_live_lat_e6      = 0;
int32_t  gps_live_lon_e6      = 0;
uint8_t  gps_live_sats        = 0;
float    gps_live_hdop        = 0.0f;
uint32_t gps_live_last_fix_ms = 0;
bool     gps_live_valid       = false;
bool     gps_live_bus_ok      = false;

// Last committed position — kept private so the distance threshold is judged
// against the previously-published value, not against drifting reads while
// stationary.
static int32_t s_last_commit_lat_e6 = 0;
static int32_t s_last_commit_lon_e6 = 0;
static bool    s_have_last_commit   = false;
static uint32_t s_last_commit_ms    = 0;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool         s_started = false;

const char *gps_profile_label(gps_profile_t p) {
    switch (p) {
        case GPS_PROFILE_WALKING: return "Walking";
        case GPS_PROFILE_CYCLING: return "Cycling";
        case GPS_PROFILE_DRIVING: return "Driving";
        case GPS_PROFILE_MANUAL:  return "Manual";
        default:                  return "?";
    }
}

static uint16_t effective_interval_s(void) {
    if (gps_custom_interval_s > 0) return gps_custom_interval_s;
    if (gps_profile >= GPS_PROFILE_COUNT) return 0;
    return s_profile_interval_s[gps_profile];
}

static uint16_t effective_distance_m(void) {
    if (gps_custom_distance_m > 0) return gps_custom_distance_m;
    if (gps_profile >= GPS_PROFILE_COUNT) return 0;
    return s_profile_distance_m[gps_profile];
}

// Approximate (planar) distance in metres between two e6 lat/lon points.
// Plenty accurate at the < ~100 m scale we care about for the commit
// threshold — saves the Haversine trig.
static float distance_m(int32_t lat_a_e6, int32_t lon_a_e6,
                        int32_t lat_b_e6, int32_t lon_b_e6) {
    const double rad   = M_PI / 180.0;
    const double earth = 6378137.0;            // metres
    double lat_avg = ((double)lat_a_e6 + (double)lat_b_e6) * 0.5 / 1e6 * rad;
    double dlat    = (double)(lat_b_e6 - lat_a_e6) / 1e6 * rad;
    double dlon    = (double)(lon_b_e6 - lon_a_e6) / 1e6 * rad;
    double x = dlon * cos(lat_avg);
    double y = dlat;
    return (float)(earth * sqrt(x * x + y * y));
}

static uint32_t now_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void publish(const gps_status_t *st) {
    portENTER_CRITICAL(&s_lock);
    gps_live_bus_ok      = st->bus_ok;
    gps_live_sats        = (uint8_t)(st->gps_sats_view + st->glo_sats_view);
    gps_live_hdop        = st->hdop;
    if (st->fix_valid) {
        gps_live_lat_e6      = st->lat_e6;
        gps_live_lon_e6      = st->lon_e6;
        gps_live_valid       = true;
        gps_live_last_fix_ms = now_ms();
    }
    portEXIT_CRITICAL(&s_lock);
}

static void task_loop(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "starting (profile=%s)", gps_profile_label(gps_profile));
    while (1) {
        uint16_t interval_s = effective_interval_s();
        if (interval_s == 0) {
            // Manual profile: sleep for a second and re-check the user's
            // configuration. No PA1010D polling happens.
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        // Allow the interval slot to be re-tunable on the fly: cap at the
        // configured heartbeat ceiling so the task wakes often enough to
        // refresh SAT / HDOP for the status strip.
        uint16_t sleep_s = interval_s;
        if (sleep_s > GPS_HEARTBEAT_MAX_S) sleep_s = GPS_HEARTBEAT_MAX_S;
        vTaskDelay(pdMS_TO_TICKS((uint32_t)sleep_s * 1000));

        gps_status_t st = {0};
        // Short timeout per poll keeps the task lean — if the chip isn't
        // ready we just try again next tick. The synchronous Auto-fill
        // path uses a much longer timeout for cold-start fixes.
        gps_read_status(1500, &st);
        publish(&st);

        if (!st.fix_valid) continue;

        uint16_t commit_m = effective_distance_m();
        uint32_t now      = now_ms();
        bool     commit;
        if (!s_have_last_commit) {
            commit = true;
        } else if (commit_m == 0) {
            commit = true;
        } else {
            float d_m = distance_m(s_last_commit_lat_e6, s_last_commit_lon_e6,
                                   st.lat_e6,             st.lon_e6);
            commit = (d_m >= (float)commit_m) ||
                     (now - s_last_commit_ms >= (uint32_t)GPS_HEARTBEAT_MAX_S * 1000u);
        }
        if (commit) {
            s_last_commit_lat_e6 = st.lat_e6;
            s_last_commit_lon_e6 = st.lon_e6;
            s_last_commit_ms     = now;
            s_have_last_commit   = true;
            ESP_LOGI(TAG, "commit lat=%ld lon=%ld sats=%u hdop=%.2f",
                     (long)st.lat_e6, (long)st.lon_e6,
                     (unsigned)gps_live_sats, (double)st.hdop);
        }
    }
}

void gps_task_start(void) {
    if (s_started) return;
    s_started = true;
    // 4 KB stack: gps_read_status uses ~1 KB of locals (line buf + chunks)
    // and a couple of Haversine doubles — room to spare. Priority 4 keeps
    // the UI responsive while the task is blocked on i2c_master_receive.
    BaseType_t r = xTaskCreate(task_loop, "gps_task", 4096, NULL, 4, NULL);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed");
        s_started = false;
    }
}
