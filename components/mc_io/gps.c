// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "gps.h"
#include <stdio.h>
#include <string.h>
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gps_parser.h"

static const char* TAG = "gps";

// QWIIC bus pinout for Tanmatsu — see [[tanmatsu-i2c-buses]] memory.
#define GPS_QWIIC_SDA  33
#define GPS_QWIIC_SCL  32
#define GPS_QWIIC_PORT 1

// Swapping to a different GPS module? The parser in gps_parser.c is generic
// NMEA-0183 (RMC/GGA/GSV, talker-agnostic), so for most modules the only
// change is the I2C address below:
//   PA1010D (MTK3333)  → 0x10  (this build)
//   Quectel L80/L86/L96 → 0x10
//   u-blox NEO-M8/M9 over DDC → 0x42
// The blind-read loop further down assumes the module streams NMEA by default
// on power-up. u-blox modules in UBX-binary mode are NOT supported -- set
// them to NMEA via UBX-CFG-PRT first, or send a one-shot config here.
#define GPS_I2C_ADDR 0x10

// Consecutive receive NACKs after which we conclude no module is present on
// the QWIIC bus and stop reading for this poll. A present PA1010D ACKs its
// address on every read (it streams NMEA, or returns padding when idle), so a
// short streak of NACKs reliably means "nothing there". Bailing early avoids
// spinning the full timeout window hammering the bus, which otherwise floods
// the console with i2c.master NACK errors on badges without a QWIIC GPS.
#define GPS_NACK_STREAK_MAX 3

// Last status from gps_read_status(), exposed via gps_last_status() so the
// Settings UI can keep showing sats / HDOP after the search toast fades.
static gps_status_t s_last;
static bool         s_last_valid = false;

// Rate-limits the "no PA1010D" warning to once per absent streak (a fresh
// warning only after a module has answered again), so a badge without a QWIIC
// GPS logs one line instead of one per poll.
static bool s_absent_logged = false;

bool gps_read_status(int timeout_ms, gps_status_t* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));

    i2c_master_bus_config_t qwiic_cfg = {
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .i2c_port                     = GPS_QWIIC_PORT,
        .scl_io_num                   = GPS_QWIIC_SCL,
        .sda_io_num                   = GPS_QWIIC_SDA,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_new_master_bus(&qwiic_cfg, &bus) != ESP_OK) {
        ESP_LOGE(TAG, "QWIIC bus init failed");
        return false;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = GPS_I2C_ADDR,
        .scl_speed_hz    = 100000,
    };
    i2c_master_dev_handle_t dev = NULL;
    if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) != ESP_OK) {
        ESP_LOGE(TAG, "add_device 0x10 failed");
        i2c_del_master_bus(bus);
        return false;
    }

    out->bus_ok = true;

    char    line_buf[160];
    size_t  line_len = 0;
    int64_t start_us = esp_timer_get_time();
    uint8_t chunk[64];
    int     nack_streak = 0;

    // A missing module NACKs every read, and the i2c.master driver logs each
    // NACK at ERROR level -- on a badge without a QWIIC GPS that alone floods
    // the console. We detect absence ourselves (nack_streak) and log one clean
    // line, so mute the driver's own NACK errors for the read and restore the
    // level afterwards.
    esp_log_level_t prev_i2c_level = esp_log_level_get("i2c.master");
    esp_log_level_set("i2c.master", ESP_LOG_NONE);

    while (1) {
        int64_t elapsed_ms = (esp_timer_get_time() - start_us) / 1000;
        if (elapsed_ms > timeout_ms) break;
        if (out->fix_valid) break;  // got a fix, no need to keep reading

        if (i2c_master_receive(dev, chunk, sizeof(chunk), 200) != ESP_OK) {
            if (++nack_streak >= GPS_NACK_STREAK_MAX) {
                if (!s_absent_logged) {
                    ESP_LOGW(TAG, "no PA1010D on QWIIC (0x%02X): %d consecutive NACKs, stopping read", GPS_I2C_ADDR,
                             nack_streak);
                    s_absent_logged = true;
                }
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(5));  // brief pause; a booting module may ACK shortly
            continue;
        }
        nack_streak     = 0;      // any successful read means the module is present
        s_absent_logged = false;  // re-arm the warning for a future disconnect

        for (size_t i = 0; i < sizeof(chunk); i++) {
            uint8_t b = chunk[i];
            if (b == '\n' || b == '\r') {
                if (line_len > 0) {
                    line_buf[line_len] = '\0';
                    if (gps_nmea_apply_line(line_buf, out)) {
                        out->sentences_seen++;
                    }
                    line_len = 0;
                }
            } else if (b >= 0x20 && b < 0x7F) {
                if (line_len < sizeof(line_buf) - 1) {
                    line_buf[line_len++] = (char)b;
                }
            }
        }
    }

    esp_log_level_set("i2c.master", prev_i2c_level);

    i2c_master_bus_rm_device(dev);
    i2c_del_master_bus(bus);

    ESP_LOGI(TAG,
             "result: bus_ok=%d sentences=%d gps_sats=%d glo_sats=%d "
             "fix_used=%d quality=%d hdop=%.2f valid=%d lat=%ld lon=%ld",
             out->bus_ok, out->sentences_seen, out->gps_sats_view, out->glo_sats_view, out->fix_used_sats,
             out->fix_quality, (double)out->hdop, out->fix_valid, (long)out->lat_e6, (long)out->lon_e6);

    if (out->sentences_seen > 0) {
        s_last       = *out;
        s_last_valid = true;
    }
    return out->sentences_seen > 0;
}

bool gps_last_status(gps_status_t* out) {
    if (!out || !s_last_valid) return false;
    *out = s_last;
    return true;
}
