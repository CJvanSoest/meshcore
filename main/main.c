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
#include "bsp/rtc.h"
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
#include "radio_system_protocol_client.h"
#include "http_server.h"
#include "wifi_connection.h"
#include "wifi_keepalive.h"
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
#include "sounds.h"

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
app_view_t current_view = VIEW_HOME;

// ── Radio (TX/RX/stats) ──────────────────────────────────────────────────────
// rx_buf + rx_count + RF stats + send_*/lora_rx_task all live in radio.c/h.
#include "radio.h"

// ── Node list ─────────────────────────────────────────────────────────────────
// Storage + update_node + build_node_display + role_label live in nodes.c/h.
#include "nodes.h"

// ── Contacts (favorites) ──────────────────────────────────────────────────────
// State + NVS persistence live in contacts.c/h.
#include "contacts.h"
#include "ble_companion.h"
#include "companion_transport.h"

// ── Chat ──────────────────────────────────────────────────────────────────────
// chat_msg_t, ring buffers, DM target, public-channel key, notification LED,
// ch_*/chat_* helpers all live in chat.c/h.
#include "chat.h"
#include "channels.h"

// field_t lives in ui_state.h (shared with input.c and render.c).

// LoRa defaults, BW choices, presets, NVS load/save, owner_name / advert_name /
// region_scope / lora_cfg / flood+direct advert interval / lora_role /
// path_hash_size all live in settings_nvs.c/h.
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
bool time_from_nvs         = false;
bool qr_overlay_active     = false;
// Shared text-edit scratch for FIELD_OWNER / FIELD_ADV_NAME / FIELD_REGION_SCOPE.
char field_edit_buf[33]    = {0};
int  field_edit_len        = 0;
bool field_editing_text    = false;
int  settings_scroll       = 0;
int  home_cursor           = 0;  // VIEW_HOME tile-grid focus (0..HOME_TILE_COUNT-1)
bool qr_from_home          = false;
char     toast_text[64]    = {0};
uint32_t toast_duration_ms  = 2000;
uint32_t toast_start_ms    = 0;
bool settings_category_list_mode = true;   // start in list when entering Settings
int  settings_category_cursor    = 0;
int  settings_category_active    = 0;

// Display blanking: F3 (yellow square) toggles the MIPI backlight off so
// the badge is silent in the pocket while keyboard input + chat LEDs remain
// live. We also stop calling render() to spare CPU + framebuffer DMA.
// The power button is owned by firmware for shutdown — F3 is unused
// elsewhere in this app so there's no conflict.
// Auto-blank: when display_blank_after_s (Settings) > 0, the same blank
// trips automatically after that many seconds of input-idle time.
static bool     display_blanked = false;
static uint32_t last_input_ms   = 0;    // updated on any input event

// Drop display backlight to 0. Keyboard backlight + RGB LED stay live so
// in-pocket notifications keep working.
static void enter_display_blank(void) {
    if (display_blanked) return;
    bsp_display_set_backlight_brightness(0);
    display_blanked = true;
}

// Restore display backlight from the persisted Settings value. Using
// apply_brightness() instead of a cached "save" var is more robust: after
// repeated blank/wake cycles a stale captured brightness could end up at
// 0, leaving the display permanently dark. apply_brightness() always pulls
// the user-configured value.
static void exit_display_blank(void) {
    if (!display_blanked) return;
    apply_brightness();
    display_blanked = false;
}

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

    // Splash header is title (TXT_TITLE=24) + attribution (TXT_SMALL=16).
    // Start diag output below both so they don't overlap.
    int  diag_y    = 74;
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
        // Boot-splash attribution so users see at a glance this is the
        // community app, not the official MeshCore iOS/Android client.
        pax_draw_text(&fb, COL_AMBER, FONT, TXT_SMALL, 14, 16 + TXT_TITLE + 4,
                      "Community app by CJ van Soest");
    }
    blit();
    vTaskDelay(pdMS_TO_TICKS(1500));

    // CET/CEST with EU DST: last Sun Mar 02:00 -> CEST(+2), last Sun Oct 03:00 -> CET(+1)
    // Must be set before settimeofday/localtime_r so all paths show local time.
    // Epoch in NVS stays UTC; tzset handles the local conversion.
    setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
    tzset();

    // wifi_connection_init_stack brings up the P4↔C6 SDIO RPC pipeline that
    // tanmatsu-lora rides on top of. We keep this call but skip the actual
    // connect step (wifi_connect_try_all) — the badge runs as a LoRa node,
    // not a WiFi client, so no scan / associate / DHCP / SNTP wastes air.
    DIAG(COL_GRAY, "wifi_connection_init_stack...");
    res = wifi_connection_init_stack();
    DIAG(res == ESP_OK ? COL_GREEN : COL_YELLOW, "  wifi init: %s (%d)",
         res == ESP_OK ? "OK" : "FAIL", res);

    // Time comes from the C6 coprocessor RTC (set by launcher/firmware SNTP
    // at boot). bsp_rtc_update_time pulls that value into the P4 system
    // clock — no app-side SNTP needed. Per Nicolai's hint.
    DIAG(COL_GRAY, "bsp_rtc_update_time...");
    esp_err_t rtc_res = bsp_rtc_update_time();
    if (rtc_res == ESP_OK) {
        identity_mark_time_synced();
        DIAG(COL_GREEN, "  RTC: synced from coprocessor");
    } else {
        DIAG(COL_YELLOW, "  RTC: FAIL (%d)", rtc_res);
    }

    // Restore last known time from NVS when no SNTP/RTC available
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
    load_ble_enabled();
    load_ble_gps_pref();
    load_advert_loc_policy();
    load_wifi();
    load_or_init_http_api_key();
    // Auto-connect to the saved WiFi network at boot, if creds are present.
    // Non-blocking: wifi_connect_try_all returns immediately on failure;
    // the Settings WiFi rows show current state.
    if (wifi_ssid[0]) {
        ESP_LOGI(TAG, "WiFi auto-connect: ssid=\"%s\"", wifi_ssid);
        wifi_connect_try_all();
    }
    // Supervisor task starts/stops the gateway-ping keepalive whenever the
    // WiFi link comes up or drops -- works for both our own auto-connect
    // and the launcher-inherited connection that happens before main.c
    // runs. Defensive: ESP-Hosted SDIO traffic alone seems to keep iOS
    // hotspots from suspending, but stricter networks (or future iOS
    // changes) might need explicit packets.
    wifi_keepalive_supervisor_start();
    http_server_supervisor_start();
    companion_transport_init();
    if (ble_enabled) {
        ble_companion_init();
    } else {
        ESP_LOGI(TAG, "BLE companion disabled in settings -- skipping NimBLE init");
    }
    load_brightness();
    apply_brightness();  // override launcher globals with our per-app NVS values
    load_sound_prefs();
    sounds_init();
    contacts_load();

    DIAG(COL_GRAY, "SD mount...");
    history_init(node_prv_key);
    DIAG(history_is_ready() ? COL_GREEN : COL_YELLOW, "  SD: %s", history_status());
    // DM history is loaded per peer in dm_select_target — no boot-time DM load.
    // Channel history is per-channel; load the active channel (Public at boot).
    if (history_is_ready()) { ch_select_channel(active_channel_idx); }

    // Restore previously-discovered nodes so the user can see + promote
    // entries from earlier sessions. Persistence task writes back every
    // ~30 s when something changed.
    if (history_is_ready()) {
        nodes_load_from_sd();
        nodes_start_save_task();
    }

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
        // Query app firmware version via radio system-protocol (event 0x05,
        // GET_INFORMATION). Upstream tanmatsu-radio >=v3.1.1 exposes this; on
        // older firmware the call NACKs/times-out and render.c falls back to
        // the hand-maintained TANMATSU_RADIO_FW_LABEL.
        radio_fw_app_version[0] = '\0';
        DIAG(COL_GRAY, "sys_proto get_information...");
        if (radio_system_protocol_init() == ESP_OK) {
            radio_system_protocol_information_t info = {0};
            if (radio_system_protocol_get_information(&info) == ESP_OK) {
                size_t n = sizeof(info.firmware_version);
                if (n > RADIO_FW_APP_VERSION_LEN - 1) n = RADIO_FW_APP_VERSION_LEN - 1;
                memcpy(radio_fw_app_version, info.firmware_version, n);
                radio_fw_app_version[n] = '\0';
                DIAG(COL_GREEN, "  fw: %s (live)", radio_fw_app_version);
            } else {
                DIAG(COL_YELLOW, "  fw: query failed - hardcoded fallback");
            }
        } else {
            DIAG(COL_YELLOW, "  fw: init failed - hardcoded fallback");
        }
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

    // Boot chime (no-op if user disabled it). Fires after the init
    // diagnostics + 3 s settle so the codec is fully ready.
    sounds_play_boot();

    render();

    last_input_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    while (1) {
        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

        // Auto-blank when idle longer than Settings → Auto-blank value. 0 = off.
        if (!display_blanked && display_blank_after_s > 0 &&
            (now_ms - last_input_ms) >= (uint32_t)display_blank_after_s * 1000u) {
            enter_display_blank();
        }

        bsp_input_event_t event;
        if (xQueueReceive(input_event_queue, &event, pdMS_TO_TICKS(1000)) != pdTRUE) {
            if (!display_blanked) {
                render();  // periodic refresh: update RX count, last-seen timers
            }
            continue;
        }

        // F3 (yellow square) toggles backlight. Only act on press-edge so
        // a single tap doesn't immediately wake the display again.
        if (event.type == INPUT_EVENT_TYPE_NAVIGATION &&
            event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_F3 &&
            event.args_navigation.state) {
            if (!display_blanked) {
                enter_display_blank();
            } else {
                exit_display_blank();
                last_input_ms = now_ms;  // re-arm idle timer on wake
                render();                // immediate redraw on wake
            }
            continue;
        }

        // While blanked, swallow keyboard/nav input so the badge is silent
        // in-pocket. Only F3 (handled above) can wake it.
        if (display_blanked) continue;

        // Any real input resets the idle timer.
        last_input_ms = now_ms;

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
