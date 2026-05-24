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
#include "history.h"
#include "settings_nvs.h"

static char const TAG[] = "main";

// ── Colors (Tokyo Night palette, matches WHY2025 cj_lora_info) ────────────────
// Existing COL_* names kept; only hex values updated so the 99 call-sites map
// to the new palette without rename churn. COL_BLACK / COL_DARK are repurposed
// as background + panel surfaces respectively.
#define COL_BLACK   0xFF1A1B26  // BG       — main background (deep indigo)
#define COL_HEADER  0xFF16161E  // HEADER   — tab bar + footer surface
#define COL_DARK    0xFF24283B  // PANEL    — row highlight / separators
#define COL_WHITE   0xFFC0CAF5  // TEXT     — body text (off-white)
#define COL_GRAY    0xFF565F89  // DIM      — secondary text
#define COL_ACCENT  0xFF7AA2F7  // BLUE     — selection / accent
#define COL_GREEN   0xFF9ECE6A  // GREEN    — ok / online
#define COL_YELLOW  0xFFE0AF68  // AMBER    — heading / warning
#define COL_RED     0xFFF7768E  // RED      — error / offline
#define COL_AMBER   COL_YELLOW   // alias for clarity in headings
#define COL_BLUE    COL_ACCENT   // alias for clarity in info rows
#define COL_PANEL   COL_DARK     // alias for clarity in surface use
#define COL_BG      COL_BLACK    // alias for clarity at root background

// ── Typography (Saira Regular: ASCII + Latin-1, variable pitch) ──────────────
#define FONT         pax_font_saira_regular
#define TXT_TITLE    24
#define TXT_TAB      22
#define TXT_BODY     20
#define TXT_SMALL    16
#define TXT_TINY     13

// ── Display globals ───────────────────────────────────────────────────────────
static size_t                     display_h_res        = 0;
static size_t                     display_v_res        = 0;
static bsp_display_color_format_t display_color_format = 0;
static bsp_display_endianness_t   display_data_endian  = 0;
static pax_buf_t                  fb                   = {0};
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

#define TAB_BAR_H      44
#define FOOTER_H       28
#define CHAT_ROW_H     44
#define CHAT_Y0        (TAB_BAR_H + 4)
#define CHAT_INPUT_H   36

// ── Settings: field identifiers ───────────────────────────────────────────────
typedef enum {
    FIELD_OWNER = 0,
    FIELD_ADV_NAME,
    FIELD_FREQ,
    FIELD_SF,
    FIELD_BW,
    FIELD_CR,
    FIELD_POWER,
    FIELD_SYNC,
    FIELD_PREAMBLE,
    FIELD_ADVERT_INT,
    FIELD_PRESET,
    FIELD_ROLE,
    FIELD_PATH_HASH_SIZE,
    FIELD_REGION_SCOPE,
    FIELD_COUNT,
} field_t;

// LoRa defaults, BW choices, presets, NVS load/save, owner_name / advert_name /
// region_scope / lora_cfg / advert_interval_s / lora_role / path_hash_size all
// live in settings_nvs.c/h.
#define NVS_LAST_TIME  "last_time_s"  // int64 SNTP timestamp (also defined in identity.c)

// ── Node identity ─────────────────────────────────────────────────────────────
// node_pub_key, node_prv_key + ready/sntp-sync flags live in identity.c/h.
#include "identity.h"
// last_advert_ms lives in radio.c.
static bool     qr_overlay_active = false;

// led_dm_pending / led_channel_pending + update_notification_led live in chat.c.

// ── App state ─────────────────────────────────────────────────────────────────
static int                           selected              = 0;
static bool                          edit_mode             = false;
static bool                          dirty                 = false;
static bool                          lora_ready            = false;
       bool                          c6_available          = false;
static bool                          radio_bootloader_mode = false;
static bool                          time_from_nvs         = false;
// Shared text-edit scratch for FIELD_OWNER / FIELD_ADV_NAME.
static char                          field_edit_buf[33]    = {0};
static int                           field_edit_len        = 0;
static bool                          field_editing_text    = false;
static int                           settings_scroll       = 0;
// node_filter lives in nodes.c/h.

// utf8_sanitize lives in chat.c (only its callers need it).

// ── Display helpers ───────────────────────────────────────────────────────────
static void blit(void) {
    esp_err_t res = bsp_display_blit(0, 0, display_h_res, display_v_res, pax_buf_get_pixels(&fb));
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "blit failed: %d", res);
    }
}

// Owner/advert/region/lora-config load/save live in settings_nvs.c.
// build_node_display lives in nodes.c.

// LoRa NVS load/save + C6 round-trip live in settings_nvs.c.
// chat_ring_add_from_disk / ch_ring_add_from_disk live in chat.c.
// send_advert / send_dm_message / send_chat_message / lora_rx_task /
// advert_task / noise_floor_task / decrypt_grp_txt all live in radio.c.

// ── Settings helpers ──────────────────────────────────────────────────────────
static int bw_index(void) {
    for (int i = 0; i < BW_COUNT; i++) {
        if (BW_OPTIONS[i] == (uint16_t)lora_cfg.bandwidth) return i;
    }
    return 7;
}

static void field_adjust(int field, int delta) {
    switch (field) {
        case FIELD_FREQ:
            if (delta > 0 && lora_cfg.frequency < 870000000u) lora_cfg.frequency += 100000;
            if (delta < 0 && lora_cfg.frequency > 863000000u) lora_cfg.frequency -= 100000;
            break;
        case FIELD_SF: {
            int sf = (int)lora_cfg.spreading_factor + delta;
            if (sf < 5)  sf = 5;
            if (sf > 12) sf = 12;
            lora_cfg.spreading_factor = (uint8_t)sf;
            break;
        }
        case FIELD_BW: {
            int idx = bw_index() + delta;
            if (idx < 0) idx = 0;
            if (idx >= BW_COUNT) idx = BW_COUNT - 1;
            lora_cfg.bandwidth = BW_OPTIONS[idx];
            break;
        }
        case FIELD_CR: {
            int cr = (int)lora_cfg.coding_rate + delta;
            if (cr < 5) cr = 5;
            if (cr > 8) cr = 8;
            lora_cfg.coding_rate = (uint8_t)cr;
            break;
        }
        case FIELD_POWER: {
            int p = (int)lora_cfg.power + delta;
            if (p < 2)  p = 2;
            if (p > 22) p = 22;
            lora_cfg.power = (uint8_t)p;
            break;
        }
        case FIELD_SYNC:
            lora_cfg.sync_word = (uint8_t)((lora_cfg.sync_word + delta) & 0xFF);
            break;
        case FIELD_PREAMBLE: {
            int pre = (int)lora_cfg.preamble_length + delta;
            if (pre < 2)     pre = 2;
            if (pre > 65535) pre = 65535;
            lora_cfg.preamble_length = (uint16_t)pre;
            break;
        }
        case FIELD_ADVERT_INT: {
            static const uint16_t presets[] = {30, 60, 300, 900};
            const int n = (int)(sizeof(presets) / sizeof(presets[0]));
            int idx = 2;  // default to 5 min
            for (int i = 0; i < n; i++) if (presets[i] == advert_interval_s) { idx = i; break; }
            idx = ((idx + delta) % n + n) % n;
            advert_interval_s = presets[idx];
            break;
        }
        case FIELD_PRESET: {
            int idx = lora_preset_match();
            if (idx < 0) {
                // Currently custom — apply first preset on +delta, last on -delta
                idx = (delta > 0) ? 0 : (LORA_PRESET_COUNT - 1);
            } else {
                idx = ((idx + delta) % LORA_PRESET_COUNT + LORA_PRESET_COUNT) % LORA_PRESET_COUNT;
            }
            lora_cfg.spreading_factor = LORA_PRESETS[idx].sf;
            lora_cfg.bandwidth        = LORA_PRESETS[idx].bw;
            lora_cfg.coding_rate      = LORA_PRESETS[idx].cr;
            break;
        }
        case FIELD_ROLE: {
            static const meshcore_device_role_t ROLES[] = {
                MESHCORE_DEVICE_ROLE_CHAT_NODE,
                MESHCORE_DEVICE_ROLE_REPEATER,
                MESHCORE_DEVICE_ROLE_ROOM_SERVER,
                MESHCORE_DEVICE_ROLE_SENSOR,
            };
            const int n = (int)(sizeof(ROLES) / sizeof(ROLES[0]));
            int idx = 0;
            for (int i = 0; i < n; i++) if (ROLES[i] == lora_role) { idx = i; break; }
            idx = ((idx + delta) % n + n) % n;
            lora_role = ROLES[idx];
            break;
        }
        case FIELD_PATH_HASH_SIZE: {
            int v = (int)path_hash_size + delta;
            if (v < 1) v = 3;
            if (v > 3) v = 1;
            path_hash_size = (uint8_t)v;
            break;
        }
        default:
            break;
    }
    dirty = true;
}

// ── Render helpers ────────────────────────────────────────────────────────────
static void render(void);

static void render_tab_bar(void) {
    int w = (int)pax_buf_get_width(&fb);
    static const char *tab_labels[VIEW_COUNT] = {"Settings", "Nodes", "DM", "Channel"};
    int tab_w = (w - 200) / VIEW_COUNT;  // reserve 200px right for status indicators

    pax_simple_rect(&fb, COL_HEADER, 0, 0, w, TAB_BAR_H);

    int label_y = (TAB_BAR_H - TXT_TAB) / 2;
    for (int i = 0; i < VIEW_COUNT; i++) {
        bool active = (i == (int)current_view);
        if (active) {
            pax_simple_rect(&fb, COL_ACCENT, i * tab_w, 0, tab_w, TAB_BAR_H);
        }
        pax_col_t col = active ? COL_HEADER : COL_GRAY;
        pax_vec2f sz  = pax_text_size(FONT, TXT_TAB, tab_labels[i]);
        int       tx  = i * tab_w + (tab_w - (int)sz.x) / 2;
        pax_draw_text(&fb, col, FONT, TXT_TAB, tx, label_y, tab_labels[i]);
    }
    // Underline whole bar
    pax_simple_rect(&fb, COL_PANEL, 0, TAB_BAR_H - 1, w, 1);

    // Right side: battery % + RX count — bump to TXT_BODY for readability
    char status_right[32] = {0};
    int  status_x         = w - 10;
    int  status_y         = (TAB_BAR_H - TXT_BODY) / 2;

    bsp_power_battery_information_t bat = {0};
    if (bsp_power_get_battery_information(&bat) == ESP_OK && bat.battery_available) {
        int pct = (int)bat.remaining_percentage;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        const char *chr = bat.battery_charging ? "+" : "";
        snprintf(status_right, sizeof(status_right), "%d%%%s", pct, chr);
        pax_col_t bat_col = pct <= 20 ? COL_RED : (pct <= 50 ? COL_AMBER : COL_GREEN);
        pax_vec2f sz = pax_text_size(FONT, TXT_BODY, status_right);
        status_x -= (int)sz.x;
        pax_draw_text(&fb, bat_col, FONT, TXT_BODY, status_x, status_y, status_right);
        status_x -= 14;
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_BODY, status_x, status_y, "|");
        status_x -= 14;
    }

    if (lora_rx_ok) {
        int cnt = 0;
        if (xSemaphoreTake(rx_mutex, 0) == pdTRUE) {
            cnt = rx_count;
            xSemaphoreGive(rx_mutex);
        }
        char rxbuf[12];
        snprintf(rxbuf, sizeof(rxbuf), "RX:%d", cnt);
        pax_vec2f sz = pax_text_size(FONT, TXT_BODY, rxbuf);
        status_x -= (int)sz.x;
        pax_draw_text(&fb, COL_GREEN, FONT, TXT_BODY, status_x, status_y, rxbuf);
    }
}

static void start_radio_bootloader(void) {
#if defined(CONFIG_IDF_TARGET_ESP32P4)
    esp_hosted_configure_heartbeat(false, 1);
    esp_hosted_deinit();
    vTaskDelay(pdMS_TO_TICKS(200));
#endif
    bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_OFF);
    vTaskDelay(pdMS_TO_TICKS(100));
    bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_BOOTLOADER);
    vTaskDelay(pdMS_TO_TICKS(500));
    render();
}

static void enter_radio_bootloader(void) {
    pax_background(&fb, COL_BG);
    pax_draw_text(&fb, COL_WHITE, FONT, TXT_TITLE, 14, 14, "Radio Firmware Update");
    pax_draw_text(&fb, COL_GRAY,  FONT, TXT_BODY,  14, 14 + TXT_TITLE + 8, "Stopping ESP-Hosted...");
    blit();
    radio_bootloader_mode = true;
    start_radio_bootloader();
}

// ── Render: bootloader screen ─────────────────────────────────────────────────
static void render_bootloader(void) {
    int w = (int)pax_buf_get_width(&fb);
    int h = (int)pax_buf_get_height(&fb);
    pax_background(&fb, COL_BG);
    pax_simple_rect(&fb, COL_ACCENT, 0, 0, w, TAB_BAR_H);
    pax_draw_text(&fb, COL_HEADER, FONT, TXT_TAB, 14, (TAB_BAR_H - TXT_TAB) / 2, "Radio Bootloader Mode");
    int y = TAB_BAR_H + 12;
    pax_draw_text(&fb, COL_GREEN, FONT, TXT_BODY, 14, y, "C6 is in bootloader mode."); y += TXT_BODY + 10;
    pax_draw_text(&fb, COL_WHITE, FONT, TXT_BODY, 14, y, "On your Mac:");               y += TXT_BODY + 6;
    pax_draw_text(&fb, COL_AMBER, pax_font_sky_mono, 14, 14, y, "ls /dev/cu.usbmodem*"); y += 20;
    pax_draw_text(&fb, COL_GRAY,  FONT,             TXT_SMALL, 14, y, "(find the new C6 USB device)"); y += TXT_SMALL + 8;
    pax_draw_text(&fb, COL_WHITE, pax_font_sky_mono, 14, 14, y, "cd tanmatsu-radio");                  y += 20;
    pax_draw_text(&fb, COL_AMBER, pax_font_sky_mono, 13, 14, y, "esptool.py --chip esp32c6");          y += 18;
    pax_draw_text(&fb, COL_AMBER, pax_font_sky_mono, 13, 14, y, "  --port /dev/cu.usbmodem21401");     y += 18;
    pax_draw_text(&fb, COL_AMBER, pax_font_sky_mono, 13, 14, y, "  --before no_reset write_flash");    y += 18;
    pax_draw_text(&fb, COL_AMBER, pax_font_sky_mono, 13, 14, y, "  --flash_mode dio --flash_freq 80m"); y += 18;
    pax_draw_text(&fb, COL_AMBER, pax_font_sky_mono, 13, 14, y, "  --flash_size 8MB");                 y += 18;
    pax_draw_text(&fb, COL_AMBER, pax_font_sky_mono, 13, 14, y, "  0x0 bootloader.bin 0x8000 pt.bin"); y += 18;
    pax_draw_text(&fb, COL_AMBER, pax_font_sky_mono, 13, 14, y, "  0xd000 ota.bin 0x10000 app.bin");
    int fy = h - FOOTER_H;
    pax_simple_rect(&fb, COL_HEADER, 0, fy, w, FOOTER_H);
    pax_simple_rect(&fb, COL_PANEL,  0, fy, w, 1);
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 12, fy + (FOOTER_H - TXT_SMALL) / 2,
                  "ESC / F1: restart badge after flashing");
    blit();
}

// ── Render: settings screen ───────────────────────────────────────────────────
static void render_settings(void) {
    int w = (int)pax_buf_get_width(&fb);
    int h = (int)pax_buf_get_height(&fb);

    pax_background(&fb, COL_BLACK);
    render_tab_bar();

    // Edit/dirty indicator in tab bar right area
    if (edit_mode) {
        const char *mode_str = "[EDIT]";
        pax_vec2f sz = pax_text_size(FONT, TXT_SMALL, mode_str);
        pax_draw_text(&fb, COL_AMBER, FONT, TXT_SMALL, w - (int)sz.x - 110,
                      (TAB_BAR_H - TXT_SMALL) / 2, mode_str);
    }

    typedef struct { const char *label; char value[64]; } row_t;
    row_t rows[FIELD_COUNT];

    snprintf(rows[FIELD_OWNER].value, sizeof(rows[FIELD_OWNER].value), "%s", owner_name);
    rows[FIELD_OWNER].label = "Owner name";

    rows[FIELD_ADV_NAME].label = "Advert name";
    snprintf(rows[FIELD_ADV_NAME].value, sizeof(rows[FIELD_ADV_NAME].value), "%s",
             lora_advert_name[0] ? lora_advert_name : "(use owner)");

    rows[FIELD_FREQ].label = "Frequency";
    snprintf(rows[FIELD_FREQ].value, sizeof(rows[FIELD_FREQ].value),
             "%.3f MHz", (double)lora_cfg.frequency / 1000000.0);

    rows[FIELD_SF].label = "Spreading factor";
    snprintf(rows[FIELD_SF].value, sizeof(rows[FIELD_SF].value), "SF%d", lora_cfg.spreading_factor);

    rows[FIELD_BW].label = "Bandwidth";
    snprintf(rows[FIELD_BW].value, sizeof(rows[FIELD_BW].value), "%d kHz", (int)lora_cfg.bandwidth);

    rows[FIELD_CR].label = "Coding rate";
    snprintf(rows[FIELD_CR].value, sizeof(rows[FIELD_CR].value), "4/%d", lora_cfg.coding_rate);

    rows[FIELD_POWER].label = "TX power";
    snprintf(rows[FIELD_POWER].value, sizeof(rows[FIELD_POWER].value), "%d dBm", (int)lora_cfg.power);

    rows[FIELD_SYNC].label = "Sync word";
    snprintf(rows[FIELD_SYNC].value, sizeof(rows[FIELD_SYNC].value), "0x%02X", (unsigned)lora_cfg.sync_word);

    rows[FIELD_PREAMBLE].label = "Preamble length";
    snprintf(rows[FIELD_PREAMBLE].value, sizeof(rows[FIELD_PREAMBLE].value), "%d", (int)lora_cfg.preamble_length);

    rows[FIELD_ADVERT_INT].label = "Advert interval";
    if (advert_interval_s < 60) {
        snprintf(rows[FIELD_ADVERT_INT].value, sizeof(rows[FIELD_ADVERT_INT].value), "%us", (unsigned)advert_interval_s);
    } else {
        snprintf(rows[FIELD_ADVERT_INT].value, sizeof(rows[FIELD_ADVERT_INT].value), "%umin", (unsigned)(advert_interval_s / 60));
    }

    rows[FIELD_PRESET].label = "LoRa preset";
    {
        int pidx = lora_preset_match();
        if (pidx >= 0) {
            snprintf(rows[FIELD_PRESET].value, sizeof(rows[FIELD_PRESET].value), "%s", LORA_PRESETS[pidx].name);
        } else {
            snprintf(rows[FIELD_PRESET].value, sizeof(rows[FIELD_PRESET].value), "(custom)");
        }
    }

    rows[FIELD_ROLE].label = "Role";
    snprintf(rows[FIELD_ROLE].value, sizeof(rows[FIELD_ROLE].value), "%s", role_label(lora_role));

    rows[FIELD_PATH_HASH_SIZE].label = "Path hash size";
    {
        static const char *hops[] = {"64 hops", "32 hops", "21 hops"};
        int hi = (path_hash_size >= 1 && path_hash_size <= 3) ? (path_hash_size - 1) : 0;
        snprintf(rows[FIELD_PATH_HASH_SIZE].value, sizeof(rows[FIELD_PATH_HASH_SIZE].value),
                 "%u byte (%s)", path_hash_size, hops[hi]);
    }

    rows[FIELD_REGION_SCOPE].label = "Region scope";
    snprintf(rows[FIELD_REGION_SCOPE].value, sizeof(rows[FIELD_REGION_SCOPE].value),
             "%s", region_scope[0] ? region_scope : "(not set)");

    const int row_h        = 44;
    const int footer_h     = 60;
    const int y0           = TAB_BAR_H + 6;
    const int list_h       = h - y0 - footer_h;
    int rows_vis           = list_h / row_h;
    if (rows_vis < 1)            rows_vis = 1;
    if (rows_vis > FIELD_COUNT)  rows_vis = FIELD_COUNT;

    if (selected < settings_scroll)             settings_scroll = selected;
    if (selected >= settings_scroll + rows_vis) settings_scroll = selected - rows_vis + 1;
    int max_scroll = FIELD_COUNT - rows_vis;
    if (max_scroll < 0)              max_scroll = 0;
    if (settings_scroll > max_scroll) settings_scroll = max_scroll;
    if (settings_scroll < 0)         settings_scroll = 0;

    int text_y_off = (row_h - TXT_BODY) / 2;

    for (int row = 0; row < rows_vis; row++) {
        int  i      = row + settings_scroll;
        int  y      = y0 + row * row_h;
        bool is_sel = (i == selected);

        if (is_sel) {
            pax_col_t bg  = edit_mode ? 0xFF3A2A1A : COL_PANEL;
            pax_col_t bar = edit_mode ? COL_AMBER  : COL_ACCENT;
            pax_simple_rect(&fb, bg,  0, y, w, row_h - 1);
            pax_simple_rect(&fb, bar, 0, y, 5, row_h - 1);
        }

        pax_simple_rect(&fb, COL_PANEL, 12, y + row_h - 1, w - 24, 1);

        pax_col_t lbl_col = is_sel ? COL_WHITE : COL_GRAY;
        pax_draw_text(&fb, lbl_col, FONT, TXT_BODY, 18, y + text_y_off, rows[i].label);

        pax_col_t val_col;
        if (i >= FIELD_FREQ && !c6_available) {
            val_col = COL_AMBER;
        } else if (is_sel && edit_mode) {
            val_col = COL_AMBER;
        } else if (is_sel) {
            val_col = COL_WHITE;
        } else {
            val_col = COL_GREEN;
        }

        char val_disp[80];
        bool is_text_field = (i == FIELD_OWNER || i == FIELD_ADV_NAME || i == FIELD_REGION_SCOPE);
        if (is_sel && edit_mode && field_editing_text && is_text_field) {
            snprintf(val_disp, sizeof(val_disp), "%s_", field_edit_buf);
        } else if (is_sel && edit_mode && !is_text_field) {
            snprintf(val_disp, sizeof(val_disp), "< %s >", rows[i].value);
        } else {
            snprintf(val_disp, sizeof(val_disp), "%s", rows[i].value);
        }
        pax_vec2f vsz = pax_text_size(FONT, TXT_BODY, val_disp);
        pax_draw_text(&fb, val_col, FONT, TXT_BODY, w - (int)vsz.x - 18, y + text_y_off, val_disp);
    }

    if (FIELD_COUNT > rows_vis) {
        char sc[40];
        snprintf(sc, sizeof(sc), "%d-%d/%d", settings_scroll + 1, settings_scroll + rows_vis, FIELD_COUNT);
        pax_vec2f sz = pax_text_size(FONT, TXT_BODY, sc);
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_BODY, w - (int)sz.x - 10, h - footer_h - TXT_BODY - 2, sc);
    }

    int fy = h - footer_h;
    pax_simple_rect(&fb, COL_HEADER, 0, fy, w, footer_h);
    pax_simple_rect(&fb, COL_PANEL, 0, fy, w, 1);

    const char *hint = NULL;
    pax_col_t   hint_col = COL_GRAY;
    if (edit_mode && field_editing_text) {
        hint = "Type to edit   Backspace: del   Enter: save   ESC: cancel";
    } else if (edit_mode) {
        hint = "Up/Down or W/S: adjust   Enter: save   ESC: cancel";
    } else if (!c6_available) {
        hint = "NVS only — C6 unavailable   U: flash radio";
        hint_col = COL_AMBER;
    } else if (selected == FIELD_OWNER) {
        hint = "Owner name is shared with launcher (Enter to edit)";
    } else if (selected == FIELD_ADV_NAME) {
        hint = "Advert name overrides owner in LoRa adverts (empty=use owner)";
    } else if (selected == FIELD_SYNC) {
        hint = "Sync word: 0x12 = public LoRa. Isolates networks.";
    } else if (selected == FIELD_PREAMBLE) {
        hint = "Preamble: longer = detects weak signal, more airtime";
    } else if (selected == FIELD_ADVERT_INT) {
        hint = "Advert interval: longer = lower duty cycle, saves battery";
    } else if (selected == FIELD_PRESET) {
        hint = "Preset overwrites SF/BW/CR. MeshCore = default net.";
    } else if (selected == FIELD_ROLE) {
        hint = "Role: advertised only. Does NOT enable repeater behavior.";
    } else {
        hint = "W/S: navigate   Enter: edit   R: reload   Tab: next   U: flash";
    }
    pax_draw_text(&fb, hint_col, FONT, TXT_BODY, 10, fy + 6, hint);

    if (dirty) {
        const char *unsaved = "* unsaved";
        pax_vec2f usz = pax_text_size(FONT, TXT_BODY, unsaved);
        pax_draw_text(&fb, COL_AMBER, FONT, TXT_BODY, w - (int)usz.x - 10, fy + 6, unsaved);
    }

    // SNTP / time status line (left) + RF stats (right) on second footer row
    int row2_y = fy + 6 + TXT_BODY + 6;
    {
        time_t    now = time(NULL);
        struct tm t;
        localtime_r(&now, &t);
        char ts[48];
        pax_col_t ts_col;
        if (identity_sntp_synced()) {
            snprintf(ts, sizeof(ts), "SNTP %02d:%02d:%02d  %02d-%02d-%04d",
                     t.tm_hour, t.tm_min, t.tm_sec,
                     t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
            ts_col = COL_GREEN;
        } else if (time_from_nvs) {
            snprintf(ts, sizeof(ts), "~%02d:%02d %02d-%02d (NVS, approx)",
                     t.tm_hour, t.tm_min, t.tm_mday, t.tm_mon + 1);
            ts_col = COL_AMBER;
        } else {
            snprintf(ts, sizeof(ts), "no time sync — timestamps incorrect");
            ts_col = COL_RED;
        }
        pax_draw_text(&fb, ts_col, FONT, TXT_BODY, 10, row2_y, ts);
    }
    {
        char rf[64];
        int  snr_dB = (int)last_rx_snr_db_x4 / 4;
        if (last_rx_stats_valid && noise_floor_valid) {
            snprintf(rf, sizeof(rf), "RX:%d SNR:%+d N:%d",
                     (int)last_rx_rssi_dbm, snr_dB, (int)noise_floor_dbm);
        } else if (last_rx_stats_valid) {
            snprintf(rf, sizeof(rf), "RX:%d SNR:%+d", (int)last_rx_rssi_dbm, snr_dB);
        } else if (noise_floor_valid) {
            snprintf(rf, sizeof(rf), "noise:%d", (int)noise_floor_dbm);
        } else {
            rf[0] = '\0';
        }
        if (rf[0]) {
            pax_vec2f rsz = pax_text_size(FONT, TXT_BODY, rf);
            pax_draw_text(&fb, COL_GRAY, FONT, TXT_BODY, w - (int)rsz.x - 10, row2_y, rf);
        }
    }

    blit();
}

// ── Render: QR overlay ────────────────────────────────────────────────────────
static void render_qr_overlay(void) {
    int w = (int)pax_buf_get_width(&fb);
    int h = (int)pax_buf_get_height(&fb);

    // Build URL
    char hex_key[65];
    for (int i = 0; i < 32; i++) snprintf(&hex_key[i * 2], 3, "%02x", node_pub_key[i]);
    hex_key[64] = '\0';

    // Use same name as send_advert: lora_advert_name overrides owner_name when set.
    const char *adv_src = lora_advert_name[0] ? lora_advert_name :
                          ((owner_name[0] && owner_name[0] != '(') ? owner_name : "");

    // URL-encode (replace spaces with +)
    char encoded_name[64];
    int ei = 0;
    for (int i = 0; adv_src[i] && ei < 62; i++) {
        char c = adv_src[i];
        if (c == ' ') { encoded_name[ei++] = '+'; }
        else          { encoded_name[ei++] = c; }
    }
    encoded_name[ei] = '\0';

    char url[200];
    snprintf(url, sizeof(url),
             "meshcore://contact/add?name=%s&public_key=%s&type=1",
             encoded_name, hex_key);

    // Generate QR code — static buffers to avoid stack overflow (~3900 bytes each)
    static uint8_t qr_data[qrcodegen_BUFFER_LEN_MAX];
    static uint8_t tmp_buf[qrcodegen_BUFFER_LEN_MAX];
    bool ok = qrcodegen_encodeText(url, tmp_buf, qr_data,
                                   qrcodegen_Ecc_MEDIUM,
                                   qrcodegen_VERSION_MIN, 10,
                                   qrcodegen_Mask_AUTO, true);

    pax_background(&fb, COL_BG);

    if (!ok) {
        pax_draw_text(&fb, COL_AMBER, FONT, TXT_BODY, 20, h / 2, "QR encode failed");
        blit();
        return;
    }

    int qr_size = qrcodegen_getSize(qr_data);
    // Scale to ~60% of screen height, centered
    int max_px  = (h * 6) / 10;
    int cell_px = max_px / qr_size;
    if (cell_px < 2) cell_px = 2;
    int qr_px   = cell_px * qr_size;
    int qr_x    = (w - qr_px) / 2;
    int qr_y    = (h - qr_px) / 2;

    // White background with quiet zone
    int margin = cell_px * 2;
    pax_simple_rect(&fb, 0xFFFFFFFF,
                    qr_x - margin, qr_y - margin,
                    qr_px + margin * 2, qr_px + margin * 2);

    // Draw modules
    for (int row = 0; row < qr_size; row++) {
        for (int col = 0; col < qr_size; col++) {
            if (qrcodegen_getModule(qr_data, col, row)) {
                pax_simple_rect(&fb, 0xFF000000,
                                qr_x + col * cell_px,
                                qr_y + row * cell_px,
                                cell_px, cell_px);
            }
        }
    }

    // Label above QR
    const char *label = "Scan to add contact";
    pax_vec2f lsz = pax_text_size(FONT, TXT_TITLE, label);
    pax_draw_text(&fb, COL_AMBER, FONT, TXT_TITLE,
                  (w - (int)lsz.x) / 2, qr_y - margin - TXT_TITLE - 6, label);

    // Name below QR
    char name_label[80];
    snprintf(name_label, sizeof(name_label), "%s  [press any key to close]",
             adv_src[0] ? adv_src : "(no name)");
    pax_vec2f nsz = pax_text_size(FONT, TXT_SMALL, name_label);
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL,
                  (w - (int)nsz.x) / 2, qr_y + qr_px + margin + 6, name_label);

    blit();
}

// ── Render: nodes screen ──────────────────────────────────────────────────────
#define NODES_ROW_H    44
#define NODES_Y0       (TAB_BAR_H + 4)
#define NODES_HEADER_H 26

static void render_nodes(void) {
    int w = (int)pax_buf_get_width(&fb);
    int h = (int)pax_buf_get_height(&fb);

    pax_background(&fb, COL_BG);
    render_tab_bar();

    int hdr_y = NODES_Y0;
    pax_simple_rect(&fb, COL_HEADER, 0, hdr_y, w, NODES_HEADER_H);
    pax_simple_rect(&fb, COL_PANEL,  0, hdr_y + NODES_HEADER_H - 1, w, 1);

    int age_col_w  = 60;
    int dist_col_w = 60;
    int pkts_col_w = 60;
    int snr_col_w  = 54;
    int rssi_col_w = 64;
    int age_hdr_x  = w - age_col_w - 6;
    int dist_hdr_x = age_hdr_x - dist_col_w;
    int pkts_hdr_x = dist_hdr_x - pkts_col_w;
    int snr_hdr_x  = pkts_hdr_x - snr_col_w;
    int rssi_hdr_x = snr_hdr_x - rssi_col_w;
    int hdr_text_y = hdr_y + (NODES_HEADER_H - TXT_SMALL) / 2;
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 8,            hdr_text_y, "Role");
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 96,           hdr_text_y, "Name");
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, rssi_hdr_x,   hdr_text_y, "RSSI");
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, snr_hdr_x,    hdr_text_y, "SNR");
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, pkts_hdr_x,   hdr_text_y, "#Pkt");
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, dist_hdr_x,   hdr_text_y, "Dist");
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, age_hdr_x,    hdr_text_y, "Seen");

    int list_y0   = NODES_Y0 + NODES_HEADER_H;
    int footer_h  = 60;
    int list_h    = h - footer_h - list_y0;
    int rows_vis  = list_h / NODES_ROW_H;
    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    int row_text_y = (NODES_ROW_H - TXT_BODY) / 2;

    if (!lora_rx_ok) {
        pax_draw_text(&fb, COL_AMBER, FONT, TXT_BODY, 12, list_y0 + 14, "LoRa radio not available");
    } else if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (node_count == 0 && contact_count == 0) {
            pax_draw_text(&fb, COL_GRAY, FONT, TXT_BODY, 12, list_y0 + 14, "Listening... no nodes heard yet.");
        } else {
            display_row_t rows_dl[MAX_CONTACTS + MAX_NODES];
            int idx_count = build_node_display(rows_dl, MAX_CONTACTS + MAX_NODES);

            int max_scroll = idx_count - rows_vis;
            if (max_scroll < 0) max_scroll = 0;
            if (node_scroll > max_scroll) node_scroll = max_scroll;
            if (node_scroll < 0)         node_scroll = 0;

            if (node_cursor >= idx_count) node_cursor = idx_count > 0 ? idx_count - 1 : 0;
            if (node_cursor < 0)          node_cursor = 0;
            if (node_cursor < node_scroll)              node_scroll = node_cursor;
            if (node_cursor >= node_scroll + rows_vis)  node_scroll = node_cursor - rows_vis + 1;

            for (int row = 0; row < rows_vis; row++) {
                int list_idx = row + node_scroll;
                if (list_idx >= idx_count) break;
                display_row_t *d = &rows_dl[list_idx];
                node_entry_t  *n = (d->node_idx >= 0) ? &node_list[d->node_idx] : NULL;

                int y = list_y0 + row * NODES_ROW_H;
                bool is_cursor = (list_idx == node_cursor);

                if (is_cursor) {
                    pax_simple_rect(&fb, COL_PANEL, 0, y, w, NODES_ROW_H);
                    pax_simple_rect(&fb, COL_ACCENT, 0, y, 5, NODES_ROW_H);
                }

                int age_x  = w - age_col_w  - 6;
                int dist_x = age_x - dist_col_w;
                int pkts_x = dist_x - pkts_col_w;
                int snr_x  = pkts_x - snr_col_w;
                int rssi_x = snr_x - rssi_col_w;

                char age_buf[20];
                if (n) {
                    uint32_t age_s = (now_ms - n->last_seen_ms) / 1000;
                    if (age_s < 60)        snprintf(age_buf, sizeof(age_buf), "%lus", (unsigned long)age_s);
                    else if (age_s < 3600) snprintf(age_buf, sizeof(age_buf), "%lum", (unsigned long)(age_s / 60));
                    else                   snprintf(age_buf, sizeof(age_buf), "%luh", (unsigned long)(age_s / 3600));
                } else {
                    snprintf(age_buf, sizeof(age_buf), "--");
                }
                pax_draw_text(&fb, COL_GRAY, FONT, TXT_BODY, age_x, y + row_text_y, age_buf);

                pax_draw_text(&fb, COL_GRAY, FONT, TXT_BODY, dist_x, y + row_text_y, "--");

                char pkts_buf[8];
                if (n) snprintf(pkts_buf, sizeof(pkts_buf), "#%d", n->packet_count);
                else   snprintf(pkts_buf, sizeof(pkts_buf), "--");
                pax_draw_text(&fb, COL_GRAY, FONT, TXT_BODY, pkts_x, y + row_text_y, pkts_buf);

                char rssi_buf[8], snr_buf[8];
                pax_col_t rssi_col, snr_col;
                if (n && n->stats_valid) {
                    int rssi_dbm = (int)n->last_rssi_dbm;
                    int snr_dB   = (int)n->last_snr_db_x4 / 4;
                    snprintf(rssi_buf, sizeof(rssi_buf), "%d", rssi_dbm);
                    snprintf(snr_buf,  sizeof(snr_buf),  "%+d", snr_dB);
                    rssi_col = (rssi_dbm >= -80)  ? COL_GREEN :
                               (rssi_dbm >= -105) ? COL_AMBER : COL_RED;
                    snr_col  = (snr_dB  >=  0)    ? COL_GREEN :
                               (snr_dB  >= -10)   ? COL_AMBER : COL_RED;
                } else {
                    snprintf(rssi_buf, sizeof(rssi_buf), "--");
                    snprintf(snr_buf,  sizeof(snr_buf),  "--");
                    rssi_col = COL_GRAY;
                    snr_col  = COL_GRAY;
                }
                pax_draw_text(&fb, rssi_col, FONT, TXT_BODY, rssi_x, y + row_text_y, rssi_buf);
                pax_draw_text(&fb, snr_col,  FONT, TXT_BODY, snr_x,  y + row_text_y, snr_buf);

                meshcore_device_role_t role = n ? n->role : (meshcore_device_role_t)contacts[d->contact_idx].role;
                const char *src_name = n ? n->name : contacts[d->contact_idx].alias;

                const char* rl = role_label(role);
                pax_col_t role_col = (role == MESHCORE_DEVICE_ROLE_REPEATER)    ? COL_BLUE :
                                     (role == MESHCORE_DEVICE_ROLE_ROOM_SERVER) ? 0xFFBB9AF7 :
                                     (role == MESHCORE_DEVICE_ROLE_SENSOR)      ? COL_AMBER :
                                                                                  COL_GREEN;
                pax_draw_text(&fb, role_col, FONT, TXT_BODY, 8, y + row_text_y, rl);

                int name_x = 96;
                if (d->is_contact) {
                    pax_draw_text(&fb, COL_AMBER, FONT, TXT_BODY, 78, y + row_text_y, "*");
                }

                char name_trunc[25];
                int  max_name_w = rssi_x - name_x - 6;
                int  max_chars  = max_name_w / 11;  // saira regular avg ~11px @20
                if (max_chars > 24) max_chars = 24;
                if (max_chars < 1)  max_chars = 1;
                snprintf(name_trunc, sizeof(name_trunc), "%.*s", max_chars, src_name);
                pax_col_t name_col = is_cursor ? COL_WHITE :
                                     (n == NULL ? COL_GRAY : COL_WHITE);
                pax_draw_text(&fb, name_col, FONT, TXT_BODY, name_x, y + row_text_y, name_trunc);

                pax_simple_rect(&fb, COL_PANEL, 12, y + NODES_ROW_H - 1, w - 24, 1);
            }

            if (idx_count > rows_vis) {
                char sc[24];
                snprintf(sc, sizeof(sc), "%d/%d", node_scroll + 1, idx_count);
                pax_vec2f sz = pax_text_size(FONT, TXT_SMALL, sc);
                pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, w - (int)sz.x - 10, h - footer_h - TXT_SMALL - 2, sc);
            }
            if (idx_count == 0 && (node_count > 0 || contact_count > 0)) {
                pax_draw_text(&fb, COL_AMBER, FONT, TXT_BODY, 12, list_y0 + 14,
                              "No entries match the active filter — press L to clear");
            }
        }
        xSemaphoreGive(node_mutex);
    }

    int fy_base  = h - footer_h;
    pax_simple_rect(&fb, COL_HEADER, 0, fy_base, w, footer_h);
    pax_simple_rect(&fb, COL_PANEL,  0, fy_base, w, 1);

    // Row 1: counts + filter pill (if active) + controls.
    int fx = 10;
    int fy_text = fy_base + 6;
    char counts[48];
    snprintf(counts, sizeof(counts), "Nodes:%d  Contacts:%d", node_count, contact_count);
    pax_vec2f csz = pax_text_size(FONT, TXT_BODY, counts);
    pax_draw_text(&fb, COL_WHITE, FONT, TXT_BODY, fx, fy_text, counts);
    fx += (int)csz.x + 20;

    if (node_filter != MESHCORE_DEVICE_ROLE_UNKNOWN) {
        char pill[40];
        snprintf(pill, sizeof(pill), "filter: %s", role_label(node_filter));
        pax_vec2f psz = pax_text_size(FONT, TXT_BODY, pill);
        // Amber pill background
        pax_simple_rect(&fb, COL_AMBER, fx - 6, fy_text - 2, (int)psz.x + 12, TXT_BODY + 4);
        pax_draw_text(&fb, COL_HEADER, FONT, TXT_BODY, fx, fy_text, pill);
        fx += (int)psz.x + 22;
    }

    const char *ctrl = (node_filter == MESHCORE_DEVICE_ROLE_UNKNOWN)
        ? "W/S nav   A:advert   F:fav   L:filter   Q:QR"
        : "L:next   F:fav   A:advert";
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, fx, fy_text + (TXT_BODY - TXT_SMALL) / 2, ctrl);

    // Row 2: advert age
    if (identity_is_ready()) {
        uint32_t now_ms2 = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        char adv_buf[48];
        if (last_advert_ms == 0) {
            snprintf(adv_buf, sizeof(adv_buf), "advert: pending");
        } else {
            uint32_t age_s = (now_ms2 - last_advert_ms) / 1000;
            snprintf(adv_buf, sizeof(adv_buf), "last advert: %lus ago", (unsigned long)age_s);
        }
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 10, fy_text + TXT_BODY + 6, adv_buf);
    }
    blit();
}

// ── Render: chat screen ───────────────────────────────────────────────────────
static void render_chat(void) {
    int w  = (int)pax_buf_get_width(&fb);
    int h  = (int)pax_buf_get_height(&fb);

    pax_background(&fb, COL_BG);
    render_tab_bar();

    // ── Inbox view: lijst van DM-gesprekken ───────────────────────────────────
    if (dm_inbox_mode) {
        int inbox_y0 = TAB_BAR_H + 6;
        int footer_h = 36;
        int inbox_h  = h - inbox_y0 - footer_h;
        int row_h    = 56;
        int rows_vis = inbox_h / row_h;
        if (rows_vis < 1) rows_vis = 1;

        // Build inbox list: optional active DM target on top + saved contacts.
        // entry: index -1 = active dm_target row, otherwise contact index.
        int idx_map[MAX_CONTACTS + 1];
        int idx_count = 0;
        bool active_on_top = dm_target_set;
        if (active_on_top) idx_map[idx_count++] = -1;
        if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            for (int i = 0; i < contact_count && idx_count < MAX_CONTACTS + 1; i++) {
                if (active_on_top && memcmp(contacts[i].pub_key, dm_target_pub, MESHCORE_PUB_KEY_SIZE) == 0)
                    continue;  // already shown as active row
                idx_map[idx_count++] = i;
            }
            xSemaphoreGive(node_mutex);
        }

        if (idx_count == 0) {
            pax_draw_text(&fb, COL_AMBER, FONT, TXT_BODY, 16, inbox_y0 + 18,
                          "No conversations yet");
            pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 16, inbox_y0 + 18 + TXT_BODY + 6,
                          "Open the Nodes tab and press Enter on a contact.");
        } else {
            if (dm_inbox_cursor >= idx_count) dm_inbox_cursor = idx_count - 1;
            if (dm_inbox_cursor < 0)          dm_inbox_cursor = 0;
            if (dm_inbox_cursor < dm_inbox_scroll)              dm_inbox_scroll = dm_inbox_cursor;
            if (dm_inbox_cursor >= dm_inbox_scroll + rows_vis)  dm_inbox_scroll = dm_inbox_cursor - rows_vis + 1;
            int max_scroll = idx_count - rows_vis;
            if (max_scroll < 0) max_scroll = 0;
            if (dm_inbox_scroll > max_scroll) dm_inbox_scroll = max_scroll;
            if (dm_inbox_scroll < 0)          dm_inbox_scroll = 0;

            for (int row = 0; row < rows_vis; row++) {
                int li = row + dm_inbox_scroll;
                if (li >= idx_count) break;
                int  e          = idx_map[li];
                bool is_active  = (e == -1);
                bool is_cursor  = (li == dm_inbox_cursor);
                const char *name;
                meshcore_device_role_t role;
                if (is_active) {
                    name = dm_target_name;
                    int ci = contact_find(dm_target_pub);
                    role = (ci >= 0) ? (meshcore_device_role_t)contacts[ci].role
                                     : MESHCORE_DEVICE_ROLE_CHAT_NODE;
                } else {
                    name = contacts[e].alias;
                    role = (meshcore_device_role_t)contacts[e].role;
                }

                int y = inbox_y0 + row * row_h;
                if (is_cursor) {
                    pax_simple_rect(&fb, COL_PANEL, 0, y, w, row_h - 2);
                    pax_simple_rect(&fb, COL_ACCENT, 0, y, 5, row_h - 2);
                }
                pax_simple_rect(&fb, COL_PANEL, 12, y + row_h - 1, w - 24, 1);

                // Avatar circle (initial letter on amber/blue background)
                int av_x = 18, av_y = y + (row_h - 36) / 2, av_d = 36;
                pax_col_t av_bg = is_active ? COL_AMBER : COL_BLUE;
                pax_simple_rect(&fb, av_bg, av_x, av_y, av_d, av_d);
                char init[2] = {(char)(name[0] ? toupper((unsigned char)name[0]) : '?'), 0};
                pax_vec2f isz = pax_text_size(FONT, TXT_TITLE, init);
                pax_draw_text(&fb, COL_HEADER, FONT, TXT_TITLE,
                              av_x + (av_d - (int)isz.x) / 2,
                              av_y + (av_d - TXT_TITLE) / 2 - 1, init);

                // Name (line 1)
                pax_col_t name_col = is_cursor ? COL_WHITE : COL_WHITE;
                pax_draw_text(&fb, name_col, FONT, TXT_BODY, av_x + av_d + 12, y + 6, name);

                // Subline (role + active marker)
                const char *rl = role_label(role);
                char sub[64];
                if (is_active) snprintf(sub, sizeof(sub), "%s  ·  active DM", rl);
                else           snprintf(sub, sizeof(sub), "%s  ·  saved contact", rl);
                pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL,
                              av_x + av_d + 12, y + 6 + TXT_BODY + 4, sub);

                // Right-side hint
                if (is_cursor) {
                    const char *cta = "Enter ›";
                    pax_vec2f sz = pax_text_size(FONT, TXT_SMALL, cta);
                    pax_draw_text(&fb, COL_AMBER, FONT, TXT_SMALL,
                                  w - (int)sz.x - 12, y + (row_h - TXT_SMALL) / 2, cta);
                }
            }

            if (idx_count > rows_vis) {
                char sc[24];
                snprintf(sc, sizeof(sc), "%d/%d", dm_inbox_cursor + 1, idx_count);
                pax_vec2f sz = pax_text_size(FONT, TXT_SMALL, sc);
                pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL,
                              w - (int)sz.x - 10, h - footer_h - TXT_SMALL - 2, sc);
            }
        }

        int fy_base = h - footer_h;
        pax_simple_rect(&fb, COL_HEADER, 0, fy_base, w, footer_h);
        pax_simple_rect(&fb, COL_PANEL,  0, fy_base, w, 1);
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 10, fy_base + (footer_h - TXT_SMALL) / 2,
                      "W/S: nav   Enter: open   D: delete   Tab: next");
        blit();
        return;
    }

    // ── Chat view: history van geselecteerde DM ───────────────────────────────
    int input_y = h - CHAT_INPUT_H - FOOTER_H;
    int list_y0 = CHAT_Y0 + 32;        // room for header with contact name
    int list_h  = input_y - list_y0;
    int rows_vis = list_h / CHAT_ROW_H;

    // Chat header bar
    pax_simple_rect(&fb, COL_PANEL, 0, CHAT_Y0, w, 28);
    {
        char hdr[MESHCORE_MAX_NAME_SIZE + 24];
        snprintf(hdr, sizeof(hdr), "‹  %s", dm_target_set ? dm_target_name : "(no target)");
        pax_draw_text(&fb, COL_WHITE, FONT, TXT_BODY, 10, CHAT_Y0 + 4, hdr);
    }

    if (xSemaphoreTake(chat_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (chat_count == 0) {
            pax_draw_text(&fb, COL_GRAY, FONT, TXT_BODY, 14, list_y0 + 10,
                          "No messages yet. Press T to type.");
        } else {
            int max_scroll = chat_count;
            if (chat_scroll > max_scroll) chat_scroll = max_scroll;
            if (chat_scroll < rows_vis)   chat_scroll = rows_vis;

            for (int row = 0; row < rows_vis; row++) {
                int msg_idx_in_list = chat_scroll - rows_vis + row;
                if (msg_idx_in_list < 0) continue;
                int ring_idx = (chat_head - chat_count + msg_idx_in_list + MAX_CHAT_MSGS * 2) % MAX_CHAT_MSGS;
                chat_msg_t* m = &chat_msgs[ring_idx];
                if (!m->active) continue;

                int y = list_y0 + row * CHAT_ROW_H;
                if (m->is_mine) {
                    pax_vec2f sz = pax_text_size(FONT, TXT_BODY, m->text);
                    int tx = w - (int)sz.x - 16;
                    if (tx < 16) tx = 16;
                    pax_simple_rect(&fb, COL_PANEL, tx - 6, y + 2, (int)sz.x + 12, CHAT_ROW_H - 8);
                    pax_draw_text(&fb, COL_BLUE, FONT, TXT_BODY, tx, y + 6, m->text);
                    pax_draw_text(&fb, COL_GRAY, FONT, TXT_TINY, tx, y + CHAT_ROW_H - TXT_TINY - 4, "You");
                } else {
                    pax_draw_text(&fb, COL_WHITE, FONT, TXT_BODY, 14, y + 6, m->text);
                }
            }
        }
        xSemaphoreGive(chat_mutex);
    }

    // Input bar
    int iy = input_y;
    pax_simple_rect(&fb, COL_PANEL, 0, iy, w, CHAT_INPUT_H);
    pax_simple_rect(&fb, chat_typing ? COL_ACCENT : COL_AMBER, 0, iy, w, 2);
    if (chat_typing) {
        char prefix[MESHCORE_MAX_NAME_SIZE + 8];
        snprintf(prefix, sizeof(prefix), "DM %s> ", dm_target_name);
        char disp[MAX_INPUT_LEN + sizeof(prefix) + 2];
        snprintf(disp, sizeof(disp), "%s%s_", prefix, chat_input);
        pax_draw_text(&fb, COL_WHITE, FONT, TXT_BODY, 10, iy + (CHAT_INPUT_H - TXT_BODY) / 2, disp);
        char ctr[12];
        snprintf(ctr, sizeof(ctr), "%d/%d", chat_input_len, MAX_INPUT_LEN);
        pax_vec2f csz = pax_text_size(FONT, TXT_SMALL, ctr);
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, w - (int)csz.x - 10, iy + (CHAT_INPUT_H - TXT_SMALL) / 2, ctr);
    } else {
        pax_draw_text(&fb, COL_AMBER, FONT, TXT_SMALL, 10, iy + (CHAT_INPUT_H - TXT_SMALL) / 2,
                      "T: type message");
    }

    // Footer
    int fy = h - FOOTER_H;
    pax_simple_rect(&fb, COL_HEADER, 0, fy, w, FOOTER_H);
    pax_simple_rect(&fb, COL_PANEL, 0, fy, w, 1);
    if (chat_typing) {
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 10, fy + (FOOTER_H - TXT_SMALL) / 2,
                      "Enter: send   ESC: cancel   Backspace: delete");
    } else {
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 10, fy + (FOOTER_H - TXT_SMALL) / 2,
                      "T: type   W/S: scroll   ESC: back to inbox   Tab: next tab");
    }
    blit();
}

// ── Render: channel screen ────────────────────────────────────────────────────
static void render_channel(void) {
    int w  = (int)pax_buf_get_width(&fb);
    int h  = (int)pax_buf_get_height(&fb);

    pax_background(&fb, COL_BG);
    render_tab_bar();

    // Header bar
    pax_simple_rect(&fb, COL_PANEL, 0, CHAT_Y0, w, 28);
    pax_draw_text(&fb, COL_WHITE, FONT, TXT_BODY, 10, CHAT_Y0 + 4, "Public channel");

    int input_y = h - CHAT_INPUT_H - FOOTER_H;
    int list_y0 = CHAT_Y0 + 32;
    int list_h  = input_y - list_y0;
    int rows_vis = list_h / CHAT_ROW_H;

    if (xSemaphoreTake(ch_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (ch_count == 0) {
            pax_draw_text(&fb, COL_GRAY, FONT, TXT_BODY, 14, list_y0 + 10,
                          "No channel messages yet. Press T to send.");
        } else {
            int max_scroll = ch_count;
            if (ch_scroll > max_scroll) ch_scroll = max_scroll;
            if (ch_scroll < rows_vis)   ch_scroll = rows_vis;

            for (int row = 0; row < rows_vis; row++) {
                int msg_idx = ch_scroll - rows_vis + row;
                if (msg_idx < 0) continue;
                int ring_idx = (ch_head - ch_count + msg_idx + MAX_CHAT_MSGS * 2) % MAX_CHAT_MSGS;
                chat_msg_t* m = &ch_msgs[ring_idx];
                if (!m->active) continue;

                int y = list_y0 + row * CHAT_ROW_H;
                if (m->is_mine) {
                    pax_vec2f sz = pax_text_size(FONT, TXT_BODY, m->text);
                    int tx = w - (int)sz.x - 16;
                    if (tx < 16) tx = 16;
                    pax_simple_rect(&fb, COL_PANEL, tx - 6, y + 2, (int)sz.x + 12, CHAT_ROW_H - 8);
                    pax_draw_text(&fb, COL_BLUE, FONT, TXT_BODY, tx, y + 6, m->text);
                    pax_draw_text(&fb, COL_GRAY, FONT, TXT_TINY, tx, y + CHAT_ROW_H - TXT_TINY - 4, "You");
                } else {
                    pax_draw_text(&fb, COL_WHITE, FONT, TXT_BODY, 14, y + 6, m->text);
                }
            }
        }
        xSemaphoreGive(ch_mutex);
    }

    // Input bar
    int iy = input_y;
    pax_simple_rect(&fb, COL_PANEL, 0, iy, w, CHAT_INPUT_H);
    pax_simple_rect(&fb, chat_typing ? COL_ACCENT : COL_GREEN, 0, iy, w, 2);
    if (chat_typing) {
        char disp[MAX_INPUT_LEN + 4];
        snprintf(disp, sizeof(disp), "> %s_", chat_input);
        pax_draw_text(&fb, COL_WHITE, FONT, TXT_BODY, 10, iy + (CHAT_INPUT_H - TXT_BODY) / 2, disp);
        char ctr[12];
        snprintf(ctr, sizeof(ctr), "%d/%d", chat_input_len, MAX_INPUT_LEN);
        pax_vec2f csz = pax_text_size(FONT, TXT_SMALL, ctr);
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, w - (int)csz.x - 10, iy + (CHAT_INPUT_H - TXT_SMALL) / 2, ctr);
    } else {
        pax_draw_text(&fb, COL_GREEN, FONT, TXT_SMALL, 10, iy + (CHAT_INPUT_H - TXT_SMALL) / 2,
                      "T: send channel message");
    }

    int fy = h - FOOTER_H;
    pax_simple_rect(&fb, COL_HEADER, 0, fy, w, FOOTER_H);
    pax_simple_rect(&fb, COL_PANEL, 0, fy, w, 1);
    if (chat_typing) {
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 10, fy + (FOOTER_H - TXT_SMALL) / 2,
                      "Enter: send   ESC: cancel   Backspace: delete");
    } else {
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 10, fy + (FOOTER_H - TXT_SMALL) / 2,
                      "T: type   W/S: scroll   R: clear history   Tab: next");
    }
    blit();
}

// ── Render dispatcher ─────────────────────────────────────────────────────────
static void render(void) {
    if (radio_bootloader_mode) {
        render_bootloader();
        return;
    }
    switch (current_view) {
        case VIEW_NODES:
            render_nodes();
            if (qr_overlay_active) render_qr_overlay();
            break;
        case VIEW_CHAT:    render_chat();    break;
        case VIEW_CHANNEL: render_channel(); break;
        case VIEW_SETTINGS:
        default:           render_settings(); break;
    }
}

// ── Input handling ────────────────────────────────────────────────────────────
// Settings: text-field edit helpers (shared between handle_nav and handle_key).
static void settings_begin_text_edit(field_t f) {
    const char *src = "";
    if (f == FIELD_OWNER && owner_name[0] && owner_name[0] != '(') src = owner_name;
    else if (f == FIELD_ADV_NAME && lora_advert_name[0])           src = lora_advert_name;
    else if (f == FIELD_REGION_SCOPE && region_scope[0])           src = region_scope;
    strncpy(field_edit_buf, src, sizeof(field_edit_buf) - 1);
    field_edit_buf[sizeof(field_edit_buf) - 1] = '\0';
    field_edit_len     = (int)strlen(field_edit_buf);
    field_editing_text = true;
}
static void settings_commit_text_edit(field_t f) {
    if (f == FIELD_OWNER) {
        strncpy(owner_name, field_edit_buf, sizeof(owner_name) - 1);
        owner_name[sizeof(owner_name) - 1] = '\0';
        save_owner_name();
    } else if (f == FIELD_ADV_NAME) {
        strncpy(lora_advert_name, field_edit_buf, sizeof(lora_advert_name) - 1);
        lora_advert_name[sizeof(lora_advert_name) - 1] = '\0';
        save_lora_advert_name();
    } else if (f == FIELD_REGION_SCOPE) {
        strncpy(region_scope, field_edit_buf, sizeof(region_scope) - 1);
        region_scope[sizeof(region_scope) - 1] = '\0';
        save_region_scope();
    }
    field_editing_text = false;
}

static void handle_nav(uint32_t key) {
    if (radio_bootloader_mode) {
        if (key == BSP_INPUT_NAVIGATION_KEY_F1 || key == BSP_INPUT_NAVIGATION_KEY_ESC) {
            bsp_led_set_mode(true);
            bsp_device_restart_to_launcher();
        }
        return;
    }

    // QR overlay: any nav key closes it
    if (qr_overlay_active) {
        qr_overlay_active = false;
        return;
    }

    if (key == BSP_INPUT_NAVIGATION_KEY_F1 || key == BSP_INPUT_NAVIGATION_KEY_ESC) {
        if (edit_mode) {
            edit_mode          = false;
            field_editing_text = false;
            dirty              = false;
            load_owner_name();
            load_lora_advert_name();
            load_region_scope();
            load_lora_config();
        } else if (chat_typing) {
            // ESC during typing is cancelled by handle_key; don't fall through to launcher restart
        } else if (current_view == VIEW_CHAT && !dm_inbox_mode) {
            // Back to inbox instead of clearing target (keeps dm_target for return)
            dm_inbox_mode = true;
        } else {
            bsp_led_set_mode(true);
            bsp_device_restart_to_launcher();
        }
    } else if (key == BSP_INPUT_NAVIGATION_KEY_UP) {
        if (current_view == VIEW_SETTINGS) {
            if (!edit_mode) selected = (selected - 1 + FIELD_COUNT) % FIELD_COUNT;
            else if (!field_editing_text) field_adjust(selected, +1);
        } else if (current_view == VIEW_NODES) {
            if (node_cursor > 0) node_cursor--;
        } else if (current_view == VIEW_CHAT && dm_inbox_mode) {
            if (dm_inbox_cursor > 0) dm_inbox_cursor--;
        }
    } else if (key == BSP_INPUT_NAVIGATION_KEY_DOWN) {
        if (current_view == VIEW_SETTINGS) {
            if (!edit_mode) selected = (selected + 1) % FIELD_COUNT;
            else if (!field_editing_text) field_adjust(selected, -1);
        } else if (current_view == VIEW_NODES) {
            int upper = node_count + contact_count - 1;
            if (upper < 0) upper = 0;
            if (node_cursor < upper) node_cursor++;
        } else if (current_view == VIEW_CHAT && dm_inbox_mode) {
            int upper = (dm_target_set ? 1 : 0) + contact_count - 1;
            if (upper < 0) upper = 0;
            if (dm_inbox_cursor < upper) dm_inbox_cursor++;
        }
    } else if (key == BSP_INPUT_NAVIGATION_KEY_LEFT) {
        if (current_view == VIEW_SETTINGS && edit_mode && !field_editing_text) field_adjust(selected, -1);
    } else if (key == BSP_INPUT_NAVIGATION_KEY_RIGHT) {
        if (current_view == VIEW_SETTINGS && edit_mode && !field_editing_text) field_adjust(selected, +1);
    } else if (key == BSP_INPUT_NAVIGATION_KEY_RETURN) {
        // Inbox: pick a conversation
        if (current_view == VIEW_CHAT && dm_inbox_mode && !chat_typing) {
            int idx_map[MAX_CONTACTS + 1];
            int idx_count = 0;
            bool active_on_top = dm_target_set;
            if (active_on_top) idx_map[idx_count++] = -1;
            for (int i = 0; i < contact_count && idx_count < MAX_CONTACTS + 1; i++) {
                if (active_on_top && memcmp(contacts[i].pub_key, dm_target_pub, MESHCORE_PUB_KEY_SIZE) == 0)
                    continue;
                idx_map[idx_count++] = i;
            }
            if (dm_inbox_cursor < idx_count) {
                int e = idx_map[dm_inbox_cursor];
                if (e >= 0) {
                    dm_select_target(contacts[e].pub_key, contacts[e].alias);
                }
                dm_inbox_mode = false;
                led_dm_pending = false;
                update_notification_led();
            }
            return;
        }
        if ((current_view == VIEW_CHAT || current_view == VIEW_CHANNEL) && chat_typing) {
            if (chat_input_len > 0) {
                if (current_view == VIEW_CHANNEL) {
                    send_chat_message(chat_input);
                    ch_add_message(chat_input, true);
                } else if (dm_target_set) {
                    send_dm_message(chat_input, dm_target_pub);
                    chat_add_dm(chat_input, true, dm_target_pub);
                    // Persist target so it stays in the inbox after reboot.
                    meshcore_device_role_t r = MESHCORE_DEVICE_ROLE_CHAT_NODE;
                    if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                        for (int ni = 0; ni < MAX_NODES; ni++) {
                            if (node_list[ni].active &&
                                memcmp(node_list[ni].pub_key, dm_target_pub, MESHCORE_PUB_KEY_SIZE) == 0) {
                                r = node_list[ni].role; break;
                            }
                        }
                        xSemaphoreGive(node_mutex);
                    }
                    contact_ensure(dm_target_pub, dm_target_name, (uint8_t)r);
                } else {
                    // No DM target: do NOT fall back to channel (was a hidden trap).
                    chat_add_message("(geen DM-target — kies een node in Nodes-tab)", false);
                }
                chat_input_len = 0;
                chat_input[0]  = '\0';
            }
            chat_typing = false;
        } else if (current_view == VIEW_NODES) {
            // Select node/contact under cursor → set DM target, open Chat tab
            if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                display_row_t rows_dl[MAX_CONTACTS + MAX_NODES];
                int idx_count = build_node_display(rows_dl, MAX_CONTACTS + MAX_NODES);
                if (node_cursor < idx_count) {
                    display_row_t *d = &rows_dl[node_cursor];
                    if (d->node_idx >= 0) {
                        node_entry_t *n = &node_list[d->node_idx];
                        dm_select_target(n->pub_key, n->name);
                        contact_ensure(n->pub_key, n->name, (uint8_t)n->role);
                    } else if (d->is_contact) {
                        contact_t *c = &contacts[d->contact_idx];
                        dm_select_target(c->pub_key, c->alias);
                    }
                }
                xSemaphoreGive(node_mutex);
            }
            if (dm_target_set) {
                current_view   = VIEW_CHAT;
                dm_inbox_mode  = false;  // straight into chat view when opened from Nodes
                led_dm_pending = false;
                update_notification_led();
            }
        } else if (current_view == VIEW_SETTINGS) {
            if (!edit_mode) {
                edit_mode = true;
                if (selected == FIELD_OWNER || selected == FIELD_ADV_NAME ||
                    selected == FIELD_REGION_SCOPE) {
                    settings_begin_text_edit(selected);
                }
            } else {
                if (field_editing_text)        settings_commit_text_edit(selected);
                else                            save_lora_config();
                edit_mode = false;
                dirty     = false;
            }
        }
    }
}

static void handle_key(char c) {
    if (radio_bootloader_mode) {
        if (c == 27) {
            bsp_led_set_mode(true);
            bsp_device_restart_to_launcher();
        }
        return;
    }

    // QR overlay: ESC closes it, all other keys ignored
    if (qr_overlay_active) {
        if (c == 27) qr_overlay_active = false;
        return;
    }

    // Settings text-edit (FIELD_OWNER / FIELD_ADV_NAME): intercept all printable
    // characters so the global W/S/T/F/L/Q/R/T shortcuts don't fire while typing.
    if (current_view == VIEW_SETTINGS && edit_mode && field_editing_text) {
        if (c == 27) {  // ESC: cancel
            edit_mode          = false;
            field_editing_text = false;
            dirty              = false;
            load_owner_name();
            load_lora_advert_name();
            load_region_scope();
        } else if (c == '\r' || c == '\n') {  // Enter: save
            settings_commit_text_edit(selected);
            edit_mode = false;
            dirty     = false;
        } else if (c == 127 || c == 8) {  // Backspace
            if (field_edit_len > 0) field_edit_buf[--field_edit_len] = '\0';
        } else if (c >= 32 && c < 127 && field_edit_len < (int)sizeof(field_edit_buf) - 1) {
            // Region scope: force lowercase, only [a-z 0-9 -] accepted
            if (selected == FIELD_REGION_SCOPE) {
                if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
                bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
                if (!ok) return;
            }
            field_edit_buf[field_edit_len++] = c;
            field_edit_buf[field_edit_len]   = '\0';
        }
        return;
    }

    // DM inbox view: own keymap (no typing here)
    if (current_view == VIEW_CHAT && dm_inbox_mode && !chat_typing) {
        if (c == 'w' || c == 'W') { if (dm_inbox_cursor > 0) dm_inbox_cursor--; return; }
        if (c == 's' || c == 'S') {
            int upper = (dm_target_set ? 1 : 0) + contact_count - 1;
            if (upper < 0) upper = 0;
            if (dm_inbox_cursor < upper) dm_inbox_cursor++;
            return;
        }
        // T to type only works inside the chat view; here we ignore it.
        if (c == 't' || c == 'T') return;
        // Tab and ESC fall through to common handling below
    }

    // Chat / Channel view input — intercept everything when typing
    if (current_view == VIEW_CHAT || current_view == VIEW_CHANNEL) {
        if (chat_typing) {
            if (c == 27) {  // ESC: cancel
                chat_typing    = false;
                chat_input_len = 0;
                chat_input[0]  = '\0';
            } else if (c == '\r' || c == '\n') {  // Enter: send
                if (chat_input_len > 0) {
                    if (current_view == VIEW_CHANNEL) {
                        send_chat_message(chat_input);
                        ch_add_message(chat_input, true);
                    } else if (dm_target_set) {
                        send_dm_message(chat_input, dm_target_pub);
                        chat_add_dm(chat_input, true, dm_target_pub);
                    } else {
                        send_chat_message(chat_input);
                        chat_add_message(chat_input, true);
                    }
                    chat_input_len = 0;
                    chat_input[0]  = '\0';
                }
                chat_typing = false;
            } else if (c == 127 || c == 8) {  // Backspace
                if (chat_input_len > 0) {
                    chat_input[--chat_input_len] = '\0';
                }
            } else if (c >= 32 && c < 127 && chat_input_len < MAX_INPUT_LEN) {
                chat_input[chat_input_len++] = c;
                chat_input[chat_input_len]   = '\0';
            }
            return;
        } else {
            if (c == 't' || c == 'T') {
                // T only enters typing mode in chat view (not inbox)
                if (current_view == VIEW_CHANNEL || (current_view == VIEW_CHAT && !dm_inbox_mode)) {
                    chat_typing = true;
                }
                return;
            }
            if (c == 'w' || c == 'W') {
                if (current_view == VIEW_CHANNEL) { if (ch_scroll > 0) ch_scroll--; }
                else                              { if (chat_scroll > 0) chat_scroll--; }
                return;
            }
            if (c == 's' || c == 'S') {
                if (current_view == VIEW_CHANNEL) ch_scroll++;
                else                              chat_scroll++;
                return;
            }
            // Tab and ESC fall through to common handling below
        }
    }

    if (c == '\t') {
        // Tab: cycle through views (not in edit mode)
        if (!edit_mode) {
            current_view = (app_view_t)((int)(current_view + 1) % VIEW_COUNT);
            if (current_view == VIEW_CHAT) {
                dm_inbox_mode      = true;   // always land on inbox when entering DM tab
                led_dm_pending     = false;
                update_notification_led();
            }
            if (current_view == VIEW_CHANNEL) { led_channel_pending = false; update_notification_led(); }
        }
        return;
    }

    if (c == 27) {
        if (edit_mode) {
            edit_mode          = false;
            field_editing_text = false;
            dirty              = false;
            load_owner_name();
            load_lora_advert_name();
            load_region_scope();
            load_lora_config();
        } else if (current_view == VIEW_CHAT && !dm_inbox_mode) {
            dm_inbox_mode = true;  // back to inbox list
        } else {
            bsp_led_set_mode(true);
            bsp_device_restart_to_launcher();
        }
    } else if (c == 'w' || c == 'W') {
        if (current_view == VIEW_SETTINGS) {
            if (!edit_mode) selected = (selected - 1 + FIELD_COUNT) % FIELD_COUNT;
            else if (!field_editing_text) field_adjust(selected, +1);
        } else if (current_view == VIEW_NODES) {
            if (node_cursor > 0) node_cursor--;
        }
    } else if (c == 's' || c == 'S') {
        if (current_view == VIEW_SETTINGS) {
            if (!edit_mode) selected = (selected + 1) % FIELD_COUNT;
            else if (!field_editing_text) field_adjust(selected, -1);
        } else if (current_view == VIEW_NODES) {
            int upper = node_count + contact_count - 1;
            if (upper < 0) upper = 0;
            if (node_cursor < upper) node_cursor++;
        }
    } else if ((c == 'a' || c == 'A') && current_view == VIEW_NODES) {
        send_advert();
    } else if ((c == 'f' || c == 'F') && current_view == VIEW_NODES) {
        // Toggle contact status of the row under the cursor
        if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            display_row_t rows_dl[MAX_CONTACTS + MAX_NODES];
            int idx_count = build_node_display(rows_dl, MAX_CONTACTS + MAX_NODES);
            if (node_cursor < idx_count) {
                display_row_t *d = &rows_dl[node_cursor];
                if (d->is_contact) {
                    contact_toggle(contacts[d->contact_idx].pub_key, NULL, 0);
                } else if (d->node_idx >= 0) {
                    node_entry_t *n = &node_list[d->node_idx];
                    int r = contact_toggle(n->pub_key, n->name, (uint8_t)n->role);
                    if (r < 0) ESP_LOGW(TAG, "Contacts list is full (%d/%d)", contact_count, MAX_CONTACTS);
                }
            }
            xSemaphoreGive(node_mutex);
        }
    } else if ((c == 'l' || c == 'L') && current_view == VIEW_NODES) {
        // Cycle filter: ALL → Chat → Repeater → Room Server → Sensor → ALL
        static const meshcore_device_role_t cycle[] = {
            MESHCORE_DEVICE_ROLE_UNKNOWN,
            MESHCORE_DEVICE_ROLE_CHAT_NODE,
            MESHCORE_DEVICE_ROLE_REPEATER,
            MESHCORE_DEVICE_ROLE_ROOM_SERVER,
            MESHCORE_DEVICE_ROLE_SENSOR,
        };
        const int n = (int)(sizeof(cycle) / sizeof(cycle[0]));
        int idx = 0;
        for (int i = 0; i < n; i++) if (cycle[i] == node_filter) { idx = i; break; }
        node_filter = cycle[(idx + 1) % n];
        node_scroll = 0;
        node_cursor = 0;
    } else if ((c == 'q' || c == 'Q') && current_view == VIEW_NODES && identity_is_ready()) {
        qr_overlay_active = true;
    } else if (c == '<' || c == ',') {
        if (current_view == VIEW_SETTINGS && edit_mode && !field_editing_text) field_adjust(selected, -1);
    } else if (c == '>' || c == '.') {
        if (current_view == VIEW_SETTINGS && edit_mode && !field_editing_text) field_adjust(selected, +1);
    } else if (c == '\r' || c == '\n') {
        if (current_view == VIEW_NODES) {
            // Select node/contact under cursor → open DM in Chat tab
            if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                display_row_t rows_dl[MAX_CONTACTS + MAX_NODES];
                int idx_count = build_node_display(rows_dl, MAX_CONTACTS + MAX_NODES);
                if (node_cursor < idx_count) {
                    display_row_t *d = &rows_dl[node_cursor];
                    if (d->node_idx >= 0) {
                        node_entry_t *n = &node_list[d->node_idx];
                        dm_select_target(n->pub_key, n->name);
                        contact_ensure(n->pub_key, n->name, (uint8_t)n->role);
                    } else if (d->is_contact) {
                        contact_t *c2 = &contacts[d->contact_idx];
                        dm_select_target(c2->pub_key, c2->alias);
                    }
                }
                xSemaphoreGive(node_mutex);
            }
            if (dm_target_set) {
                current_view   = VIEW_CHAT;
                dm_inbox_mode  = false;  // straight into chat view when opened from Nodes
                led_dm_pending = false;
                update_notification_led();
            }
        } else if (current_view == VIEW_SETTINGS) {
            if (!edit_mode) {
                edit_mode = true;
                if (selected == FIELD_OWNER || selected == FIELD_ADV_NAME ||
                    selected == FIELD_REGION_SCOPE) {
                    settings_begin_text_edit(selected);
                }
            } else {
                if (field_editing_text)        settings_commit_text_edit(selected);
                else                            save_lora_config();
                edit_mode = false;
                dirty     = false;
            }
        }
    } else if (c == 'r' || c == 'R') {
        if (current_view == VIEW_SETTINGS) {
            load_owner_name();
            load_lora_advert_name();
            load_region_scope();
            load_lora_config();
            dirty              = false;
            edit_mode          = false;
            field_editing_text = false;
        } else if (current_view == VIEW_CHANNEL && !chat_typing) {
            // Wipe channel history (RAM ring + on-disk file)
            if (xSemaphoreTake(ch_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                memset(ch_msgs, 0, sizeof(ch_msgs));
                ch_head    = 0;
                ch_count   = 0;
                ch_scroll  = 0;
                xSemaphoreGive(ch_mutex);
            }
            history_delete_channel();
            ESP_LOGI(TAG, "Channel history cleared by user (R)");
        }
    } else if ((c == 'd' || c == 'D') && current_view == VIEW_CHAT && dm_inbox_mode && !chat_typing) {
        // Delete selected DM conversation: history file + contact from NVS.
        // Contact is re-added automatically on next DM from that peer.
        int idx_map[MAX_CONTACTS + 1];
        int idx_count = 0;
        bool active_on_top = dm_target_set;
        if (active_on_top) idx_map[idx_count++] = -1;
        for (int i = 0; i < contact_count && idx_count < MAX_CONTACTS + 1; i++) {
            if (active_on_top && memcmp(contacts[i].pub_key, dm_target_pub, MESHCORE_PUB_KEY_SIZE) == 0)
                continue;
            idx_map[idx_count++] = i;
        }
        if (dm_inbox_cursor < idx_count) {
            int e = idx_map[dm_inbox_cursor];
            uint8_t target_pub[MESHCORE_PUB_KEY_SIZE];
            char    target_name[MESHCORE_MAX_NAME_SIZE + 1];
            int     ci = -1;
            if (e == -1) {
                memcpy(target_pub, dm_target_pub, MESHCORE_PUB_KEY_SIZE);
                strncpy(target_name, dm_target_name, sizeof(target_name) - 1);
                target_name[sizeof(target_name) - 1] = '\0';
                ci = contact_find(dm_target_pub);
                dm_target_set = false;
                memset(chat_msgs, 0, sizeof(chat_msgs));
                chat_head = chat_count = chat_scroll = 0;
            } else {
                ci = e;
                memcpy(target_pub, contacts[ci].pub_key, MESHCORE_PUB_KEY_SIZE);
                strncpy(target_name, contacts[ci].alias, sizeof(target_name) - 1);
                target_name[sizeof(target_name) - 1] = '\0';
            }

            history_delete_dm(target_pub);
            if (ci >= 0) {
                for (int j = ci; j < contact_count - 1; j++) contacts[j] = contacts[j + 1];
                memset(&contacts[contact_count - 1], 0, sizeof(contact_t));
                contact_count--;
                contacts_save();
            }
            if (dm_inbox_cursor > 0) dm_inbox_cursor--;
            ESP_LOGI(TAG, "DM deleted by user (D): %s", target_name);
        }
    } else if ((c == 'u' || c == 'U') && !edit_mode && current_view == VIEW_SETTINGS) {
        enter_radio_bootloader();
    }
}

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
    identity_init();

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
    pax_draw_text(&fb, COL_AMBER, FONT, TXT_TITLE, 14, 16, "MeshCore v4");
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
    contacts_load();

    DIAG(COL_GRAY, "SD mount...");
    history_init(node_prv_key);
    DIAG(history_is_ready() ? COL_GREEN : COL_YELLOW, "  SD: %s", history_status());
    // DM history is loaded per peer in dm_select_target — no boot-time DM load.
    if (history_is_ready()) { history_load_channel(ch_ring_add_from_disk); }

    DIAG(COL_GRAY, "lora_init(16)...");
    res = lora_init(16);
    DIAG(res == ESP_OK ? COL_GREEN : COL_RED, "  lora_init: %s (%d)",
         res == ESP_OK ? "OK" : "FAIL", res);

    load_lora_from_nvs();
    lora_ready = true;
    DIAG(COL_GREEN, "NVS: %.3fMHz SF%d BW%d",
         (double)lora_cfg.frequency / 1000000.0, lora_cfg.spreading_factor, (int)lora_cfg.bandwidth);

    if (res == ESP_OK) {
        DIAG(COL_GRAY, "lora_get_config from C6...");
        lora_protocol_config_params_t c6_cfg = {0};
        esp_err_t cfg_res = lora_get_config(&c6_cfg);
        if (cfg_res == ESP_OK) {
            c6_available = true;
            if (c6_cfg.frequency != 0) {
                lora_cfg = c6_cfg;
                save_lora_to_nvs();
                DIAG(COL_GREEN, "  C6 OK! %.3fMHz SF%d",
                     (double)lora_cfg.frequency / 1000000.0, lora_cfg.spreading_factor);
            } else {
                DIAG(COL_YELLOW, "  C6 fresh - pushing NVS config");
                lora_set_config(&lora_cfg);
            }

            // Set RX mode and start background task
            DIAG(COL_GRAY, "lora_set_mode(RX)...");
            esp_err_t mode_res = lora_set_mode(LORA_PROTOCOL_MODE_RX);
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
        DIAG(COL_YELLOW, "lora_init failed - NVS values only");
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
