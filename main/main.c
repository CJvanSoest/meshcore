/*
 * MeshCore for Tanmatsu
 *
 * A MeshCore LoRa mesh communication app for the Tanmatsu badge (ESP32-P4).
 * Supports DM (direct messages), public channel chat, node discovery,
 * LoRa radio configuration, and QR-code based contact sharing.
 *
 * SPDX-FileCopyrightText: 2026 CJ van Soest
 * SPDX-License-Identifier: MIT
 *
 * Developed with Claude AI (Anthropic) as AI co-author.
 *
 * Third-party components:
 *   qrcodegen.{c,h}  — © Project Nayuki, MIT License
 *                       https://www.nayuki.io/page/qr-code-generator-library
 *   ed25519.{c,h}    — Based on public domain NaCl/SUPERCOP ref10
 *                       (D.J. Bernstein et al.); ESP32 adaptation MIT License
 *   meshcore/        — © 2025 Scott Powell / rippleradios.com,
 *                       © 2025 Nicolai Electronics, MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <ctype.h>
#include <time.h>
#include <sys/time.h>
#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "bsp/led.h"
#include "bsp/power.h"
#include "bsp/tanmatsu.h"
#include "tanmatsu_coprocessor.h"
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"
#include "pax_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lora.h"
#include "wifi_connection.h"
#include "meshcore/packet.h"
#include "meshcore/payload/advert.h"
#include "meshcore/payload/grp_txt.h"
#include "mbedtls/aes.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include "esp_random.h"
#include "esp_sntp.h"
#include "ed25519.h"
#include "qrcodegen.h"
#if defined(CONFIG_IDF_TARGET_ESP32P4)
#include "esp_hosted.h"
#endif

#include "app_config.h"
#include "emoji.h"
#include "history.h"
#include "input.h"
#include "settings_nvs.h"

static char const TAG[] = "main";

// COL_* palette, FONT/TXT_* sizes, TAB_BAR_H / FOOTER_H / CHAT_* layout
// constants, blit(), render(), and the display_h_res / display_v_res / fb
// globals all live in render.c/h.
#include "render.h"

static bsp_display_color_format_t display_color_format = 0;
static bsp_display_endianness_t   display_data_endian  = 0;
static QueueHandle_t              input_event_queue    = NULL;

// ── Views ─────────────────────────────────────────────────────────────────────
// app_view_t + current_view live in app_config.h (shared across modules).
app_view_t current_view = VIEW_SETTINGS;

// ── Radio (TX/RX/stats) ──────────────────────────────────────────────────────
// rx_buf + rx_count + RF stats + send_*/lora_rx_task all live in radio.c/h.
#include "radio.h"

// ── Node list ─────────────────────────────────────────────────────────────────
// Storage + update_node + build_node_display + role_label live in nodes.c/h.
#include "nodes.h"

// ── Contacts (favorites) ──────────────────────────────────────────────────────
// State + NVS persistence live in contacts.c/h.
#include "contacts.h"

// ── Chat ──────────────────────────────────────────────────────────────────────
// chat_msg_t, ring buffers, DM target, public-channel key, notification LED,
// ch_*/chat_* helpers all live in chat.c/h.
#include "chat.h"
#include "channels.h"

// field_t lives in ui_state.h (shared with input.c and render.c).

// LoRa defaults, BW choices, presets, NVS load/save, owner_name / advert_name /
// region_scope / lora_cfg / advert_interval_s / lora_role / path_hash_size all
// live in settings_nvs.c/h.
#define NVS_LAST_TIME  "last_time_s"  // int64 SNTP timestamp (also defined in identity.c)

// ── Node identity ─────────────────────────────────────────────────────────────
// node_pub_key, node_prv_key + ready/sntp-sync flags live in identity.c/h.
#include "identity.h"
// last_advert_ms lives in radio.c.
// led_dm_pending / led_channel_pending + update_notification_led live in chat.c.

// ── App state (definitions for externs declared in ui_state.h) ───────────────
#include "ui_state.h"
int  selected              = 0;
bool edit_mode             = false;
bool dirty                 = false;
bool lora_ready            = false;
bool c6_available          = false;
bool radio_bootloader_mode = false;
bool time_from_nvs         = false;
bool qr_overlay_active     = false;
// Shared text-edit scratch for FIELD_OWNER / FIELD_ADV_NAME / FIELD_REGION_SCOPE.
char field_edit_buf[33]    = {0};
int  field_edit_len        = 0;
bool field_editing_text    = false;
int  settings_scroll       = 0;

// utf8_sanitize lives in chat.c. node_filter lives in nodes.c.

// ── app_main ──────────────────────────────────────────────────────────────────
void app_main(void) {
    gpio_install_isr_service(0);

    esp_err_t res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES || res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        res = nvs_flash_init();
    }
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %d", res);
        return;
    }

    const bsp_configuration_t bsp_cfg = {
        .display = {
            .requested_color_format = BSP_DISPLAY_COLOR_FORMAT_24_888RGB,
            .num_fbs                = 1,
        },
    };
    res = bsp_device_initialize(&bsp_cfg);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "BSP init failed: %d", res);
        return;
    }

    res = bsp_display_get_parameters(&display_h_res, &display_v_res, &display_color_format, &display_data_endian);
    if (res != ESP_ERR_NOT_SUPPORTED && res == ESP_OK) {
        pax_buf_type_t fmt = PAX_BUF_24_888RGB;
        switch (display_color_format) {
            case BSP_DISPLAY_COLOR_FORMAT_16_565RGB:   fmt = PAX_BUF_16_565RGB;   break;
            case BSP_DISPLAY_COLOR_FORMAT_32_8888ARGB: fmt = PAX_BUF_32_8888ARGB; break;
            default: break;
        }
        bsp_display_rotation_t rot = bsp_display_get_default_rotation();
        pax_orientation_t ori = PAX_O_UPRIGHT;
        switch (rot) {
            case BSP_DISPLAY_ROTATION_90:  ori = PAX_O_ROT_CCW;  break;
            case BSP_DISPLAY_ROTATION_180: ori = PAX_O_ROT_HALF; break;
            case BSP_DISPLAY_ROTATION_270: ori = PAX_O_ROT_CW;   break;
            default: break;
        }
        pax_buf_init(&fb, NULL, display_h_res, display_v_res, fmt);
        pax_buf_reversed(&fb, display_data_endian == BSP_DISPLAY_ENDIAN_BIG);
        pax_buf_set_orientation(&fb, ori);
    }

    ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));

    // Module init (radio_start_tasks below creates rx_mutex when tasks come up).
    nodes_init();
    chat_init();
    channels_init();
    identity_init();
    emoji_init();

    int  diag_y    = 50;
    int  diag_line = 22;
#define DIAG(col, fmt, ...) do { \
        char _buf[80]; \
        snprintf(_buf, sizeof(_buf), fmt, ##__VA_ARGS__); \
        ESP_LOGI(TAG, "%s", _buf); \
        pax_draw_text(&fb, (col), FONT, TXT_SMALL, 14, diag_y, _buf); \
        diag_y += diag_line; \
        blit(); \
    } while(0)

    pax_background(&fb, COL_BG);
    {
        const esp_app_desc_t *desc = esp_app_get_description();
        char title[48];
        snprintf(title, sizeof(title), "MeshCore %s",
                 (desc && desc->version[0]) ? desc->version : "?");
        pax_draw_text(&fb, COL_AMBER, FONT, TXT_TITLE, 14, 16, title);
    }
    blit();
    vTaskDelay(pdMS_TO_TICKS(1500));

    // CET/CEST with EU DST: last Sun Mar 02:00 -> CEST(+2), last Sun Oct 03:00 -> CET(+1)
    // Must be set before settimeofday/localtime_r so all paths show local time.
    // Epoch in NVS stays UTC; tzset handles the local conversion.
    setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
    tzset();

    DIAG(COL_GRAY, "wifi_connection_init_stack...");
    res = wifi_connection_init_stack();
    DIAG(res == ESP_OK ? COL_GREEN : COL_YELLOW, "  wifi init: %s (%d)",
         res == ESP_OK ? "OK" : "FAIL", res);
    if (res == ESP_OK) {
        res = wifi_connect_try_all();
        DIAG(res == ESP_OK ? COL_GREEN : COL_YELLOW, "  wifi connect: %s",
             res == ESP_OK ? "OK" : "no saved networks");
        if (res == ESP_OK) {
            esp_sntp_set_time_sync_notification_cb(identity_sntp_sync_cb);
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_init();
        }
    }

    // Restore last known time from NVS when no WiFi/SNTP available
    if (!identity_sntp_synced()) {
        nvs_handle_t h;
        if (nvs_open("system", NVS_READONLY, &h) == ESP_OK) {
            int64_t saved = 0;
            if (nvs_get_i64(h, NVS_LAST_TIME, &saved) == ESP_OK && saved > 1000000000LL) {
                struct timeval tv = { .tv_sec = (time_t)saved, .tv_usec = 0 };
                settimeofday(&tv, NULL);
                time_from_nvs = true;
                DIAG(COL_YELLOW, "  time: NVS restore (approx)");
            }
            nvs_close(h);
        }
    }

    load_owner_name();
    load_lora_advert_name();
    load_region_scope();
    load_country_code();
    load_antenna_gain();
    load_gps_coords();
    contacts_load();

    DIAG(COL_GRAY, "SD mount...");
    history_init(node_prv_key);
    DIAG(history_is_ready() ? COL_GREEN : COL_YELLOW, "  SD: %s", history_status());
    // DM history is loaded per peer in dm_select_target — no boot-time DM load.
    // Channel history is per-channel; load the active channel (Public at boot).
    if (history_is_ready()) { ch_select_channel(active_channel_idx); }

    DIAG(COL_GRAY, "lora_init_remote(16)...");
    res = lora_init_remote(&lora_handle, 16);
    DIAG(res == ESP_OK ? COL_GREEN : COL_RED, "  lora_init_remote: %s (%d)",
         res == ESP_OK ? "OK" : "FAIL", res);

    load_lora_from_nvs();
    lora_ready = true;
    DIAG(COL_GREEN, "NVS: %.3fMHz SF%d BW%d",
         (double)lora_cfg.frequency / 1000000.0, lora_cfg.spreading_factor, (int)lora_cfg.bandwidth);

    if (res == ESP_OK) {
        // Query radio firmware version once at boot (for display in Settings).
        lora_protocol_status_params_t status = {0};
        if (lora_get_status(&lora_handle, &status) == ESP_OK) {
            // status.version_string is fixed-length, may not be null-terminated.
            size_t n = sizeof(status.version_string);
            if (n > RADIO_FW_VERSION_LEN - 1) n = RADIO_FW_VERSION_LEN - 1;
            memcpy(radio_fw_version, status.version_string, n);
            radio_fw_version[n] = '\0';
        }
        // Radio app-firmware version query dropped in the v0.2.1 sync: this C6
        // firmware carries the upstream system-protocol (stub on our fork base);
        // re-add a query via the system protocol once it exposes a version cmd.
        // radio_fw_app_version stays empty → render.c shows TANMATSU_RADIO_FW_LABEL.
        radio_fw_app_version[0] = '\0';
        DIAG(COL_GRAY, "lora_get_config from C6...");
        lora_protocol_config_params_t c6_cfg = {0};
        esp_err_t cfg_res = lora_get_config(&lora_handle, &c6_cfg);
        if (cfg_res == ESP_OK) {
            c6_available = true;
            if (c6_cfg.frequency != 0) {
                lora_cfg = c6_cfg;
                save_lora_to_nvs();
                DIAG(COL_GREEN, "  C6 OK! %.3fMHz SF%d",
                     (double)lora_cfg.frequency / 1000000.0, lora_cfg.spreading_factor);
            } else {
                DIAG(COL_YELLOW, "  C6 fresh - pushing NVS config");
                lora_set_config(&lora_handle, &lora_cfg);
            }

            // Set RX mode and start background task
            DIAG(COL_GRAY, "lora_set_mode(RX)...");
            esp_err_t mode_res = lora_set_mode(&lora_handle, LORA_PROTOCOL_MODE_RX);
            if (mode_res == ESP_OK) {
                lora_rx_ok = true;
                DIAG(COL_GREEN, "  RX mode OK - starting tasks");
                radio_start_tasks();
            } else {
                DIAG(COL_YELLOW, "  RX mode failed (%d)", mode_res);
            }
        } else {
            DIAG(COL_YELLOW, "  C6 unavail (err=%d) - NVS only", cfg_res);
        }
    } else {
        DIAG(COL_YELLOW, "lora_init_remote failed - NVS values only");
    }

    vTaskDelay(pdMS_TO_TICKS(3000));
#undef DIAG

    render();

    while (1) {
        bsp_input_event_t event;
        if (xQueueReceive(input_event_queue, &event, pdMS_TO_TICKS(1000)) != pdTRUE) {
            render();  // periodic refresh: update RX count, last-seen timers
            continue;
        }

        bool changed = false;

        if (event.type == INPUT_EVENT_TYPE_NAVIGATION && event.args_navigation.state) {
            handle_nav(event.args_navigation.key);
            changed = true;
        } else if (event.type == INPUT_EVENT_TYPE_KEYBOARD) {
            handle_key(event.args_keyboard.ascii);
            changed = true;
        }

        if (changed) {
            render();
        }
    }
}
