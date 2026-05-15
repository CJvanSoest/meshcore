#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "bsp/led.h"
#include "bsp/power.h"
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
#include "lora.h"
#include "wifi_connection.h"
#if defined(CONFIG_IDF_TARGET_ESP32P4)
#include "esp_hosted.h"
#endif

static char const TAG[] = "main";

// Colors
#define COL_BLACK   0xFF000000
#define COL_WHITE   0xFFFFFFFF
#define COL_GRAY    0xFF888888
#define COL_DARK    0xFF222222
#define COL_ACCENT  0xFF0088FF
#define COL_GREEN   0xFF00BB44
#define COL_YELLOW  0xFFFFCC00
#define COL_RED     0xFFFF4444

// Display globals
static size_t                     display_h_res        = 0;
static size_t                     display_v_res        = 0;
static bsp_display_color_format_t display_color_format = 0;
static bsp_display_endianness_t   display_data_endian  = 0;
static pax_buf_t                  fb                   = {0};
static QueueHandle_t              input_event_queue    = NULL;

// Field identifiers
typedef enum {
    FIELD_OWNER = 0,
    FIELD_FREQ,
    FIELD_SF,
    FIELD_BW,
    FIELD_CR,
    FIELD_POWER,
    FIELD_SYNC,
    FIELD_PREAMBLE,
    FIELD_COUNT,
} field_t;

// BW options for SX1262 (kHz)
static const uint16_t BW_OPTIONS[] = {7, 10, 15, 20, 31, 41, 62, 125, 250, 500};
static const int      BW_COUNT     = (int)(sizeof(BW_OPTIONS) / sizeof(BW_OPTIONS[0]));

// NVS keys for LoRa — same namespace/keys as launcher so settings are shared
#define NVS_LORA_FREQ  "lora.freq"
#define NVS_LORA_SF    "lora.sf"
#define NVS_LORA_BW    "lora.bandwidth"
#define NVS_LORA_CR    "lora.codingrate"
#define NVS_LORA_POWER "lora.power"

// Launcher defaults (used when NVS is empty)
#define LORA_DEF_FREQ     869618000u
#define LORA_DEF_SF       8
#define LORA_DEF_BW       62
#define LORA_DEF_CR       8
#define LORA_DEF_POWER    22
#define LORA_DEF_SYNC     0x12
#define LORA_DEF_PREAMBLE 16
#define LORA_DEF_RAMP     40

// App state
static int                           selected              = 0;
static bool                          edit_mode             = false;
static bool                          dirty                 = false;
static bool                          lora_ready            = false;  // NVS or C6 values loaded
static bool                          c6_available          = false;  // C6 actually responding
static bool                          radio_bootloader_mode = false;
static char                          owner_name[33] = {0};
static lora_protocol_config_params_t lora_cfg       = {0};

static void blit(void) {
    esp_err_t res = bsp_display_blit(0, 0, display_h_res, display_v_res, pax_buf_get_pixels(&fb));
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "blit failed: %d", res);
    }
}

static void load_owner_name(void) {
    nvs_handle_t handle;
    esp_err_t res = nvs_open("system", NVS_READONLY, &handle);
    if (res != ESP_OK) {
        snprintf(owner_name, sizeof(owner_name), "(no NVS)");
        return;
    }
    size_t len = sizeof(owner_name) - 1;
    res = nvs_get_str(handle, "owner.nickname", owner_name, &len);
    if (res != ESP_OK) {
        snprintf(owner_name, sizeof(owner_name), "(not set)");
    }
    nvs_close(handle);
}

static void save_owner_name(void) {
    nvs_handle_t handle;
    esp_err_t res = nvs_open("system", NVS_READWRITE, &handle);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "NVS open for write failed: %d", res);
        return;
    }
    nvs_set_str(handle, "owner.nickname", owner_name);
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "Owner name saved: %s", owner_name);
}

static void load_lora_from_nvs(void) {
    // Apply launcher defaults first
    lora_cfg.frequency                  = LORA_DEF_FREQ;
    lora_cfg.spreading_factor           = LORA_DEF_SF;
    lora_cfg.bandwidth                  = LORA_DEF_BW;
    lora_cfg.coding_rate                = LORA_DEF_CR;
    lora_cfg.power                      = LORA_DEF_POWER;
    lora_cfg.sync_word                  = LORA_DEF_SYNC;
    lora_cfg.preamble_length            = LORA_DEF_PREAMBLE;
    lora_cfg.ramp_time                  = LORA_DEF_RAMP;
    lora_cfg.crc_enabled                = true;
    lora_cfg.invert_iq                  = false;
    lora_cfg.low_data_rate_optimization = false;

    nvs_handle_t handle;
    if (nvs_open("system", NVS_READONLY, &handle) != ESP_OK) return;
    uint32_t freq = 0;
    uint8_t  sf = 0, cr = 0, power = 0;
    uint16_t bw = 0;
    if (nvs_get_u32(handle, NVS_LORA_FREQ,  &freq)  == ESP_OK && freq  != 0) lora_cfg.frequency        = freq;
    if (nvs_get_u8 (handle, NVS_LORA_SF,    &sf)    == ESP_OK && sf    != 0) lora_cfg.spreading_factor = sf;
    if (nvs_get_u16(handle, NVS_LORA_BW,    &bw)    == ESP_OK && bw    != 0) lora_cfg.bandwidth        = bw;
    if (nvs_get_u8 (handle, NVS_LORA_CR,    &cr)    == ESP_OK && cr    != 0) lora_cfg.coding_rate      = cr;
    if (nvs_get_u8 (handle, NVS_LORA_POWER, &power) == ESP_OK)               lora_cfg.power            = power;
    nvs_close(handle);
}

static void save_lora_to_nvs(void) {
    nvs_handle_t handle;
    if (nvs_open("system", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u32(handle, NVS_LORA_FREQ,  lora_cfg.frequency);
    nvs_set_u8 (handle, NVS_LORA_SF,   lora_cfg.spreading_factor);
    nvs_set_u16(handle, NVS_LORA_BW,   (uint16_t)lora_cfg.bandwidth);
    nvs_set_u8 (handle, NVS_LORA_CR,   lora_cfg.coding_rate);
    nvs_set_u8 (handle, NVS_LORA_POWER, lora_cfg.power);
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "LoRa config saved to NVS");
}

static void load_lora_config(void) {
    // Always load from NVS first (works even if C6 is unavailable)
    load_lora_from_nvs();
    lora_ready   = true;
    c6_available = false;

    // Try C6 — availability is determined by ESP_OK, not by config values
    lora_protocol_config_params_t c6_cfg = {0};
    esp_err_t res = lora_get_config(&c6_cfg);
    if (res == ESP_OK) {
        c6_available = true;
        if (c6_cfg.frequency != 0) {
            // C6 has valid config — use it as authoritative and sync NVS
            lora_cfg = c6_cfg;
            save_lora_to_nvs();
            ESP_LOGI(TAG, "LoRa config from C6: freq=%luHz sf=%d", (unsigned long)lora_cfg.frequency, lora_cfg.spreading_factor);
        } else {
            // C6 is fresh (empty config) — push NVS values to initialize it
            ESP_LOGI(TAG, "C6 has empty config, pushing NVS values to C6");
            lora_set_config(&lora_cfg);
        }
    } else {
        ESP_LOGW(TAG, "C6 unavailable (err=%d) — using NVS values", res);
    }
}

static void save_lora_config(void) {
    save_lora_to_nvs();  // always persist to NVS
    if (c6_available) {
        esp_err_t res = lora_set_config(&lora_cfg);
        if (res != ESP_OK) {
            ESP_LOGE(TAG, "lora_set_config failed: %d", res);
        } else {
            ESP_LOGI(TAG, "LoRa config pushed to C6");
        }
    }
}

static int bw_index(void) {
    for (int i = 0; i < BW_COUNT; i++) {
        if (BW_OPTIONS[i] == (uint16_t)lora_cfg.bandwidth) return i;
    }
    return 7; // default: 125 kHz
}

static void field_adjust(int field, int delta) {
    switch (field) {
        case FIELD_FREQ:
            // Step in 100 kHz increments, range 863–870 MHz for EU868
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
        default:
            break;
    }
    dirty = true;
}

static void render(void);

static void start_radio_bootloader(void) {
    // Stop ESP-Hosted cleanly so P4 doesn't restart when C6 goes offline
#if defined(CONFIG_IDF_TARGET_ESP32P4)
    esp_hosted_configure_heartbeat(false, 1);
    esp_hosted_deinit();
    vTaskDelay(pdMS_TO_TICKS(200));
#endif
    bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_OFF);
    vTaskDelay(pdMS_TO_TICKS(100));
    bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_BOOTLOADER);
    vTaskDelay(pdMS_TO_TICKS(500));
    render();  // show "C6 in bootloader, flash via USB" screen
}

static void enter_radio_bootloader(void) {
    pax_background(&fb, COL_BLACK);
    pax_draw_text(&fb, COL_WHITE, pax_font_sky_mono, 18, 10, 10, "Radio Firmware Update");
    pax_draw_text(&fb, COL_GRAY,  pax_font_sky_mono, 14, 10, 48, "Stopping ESP-Hosted...");
    blit();
    radio_bootloader_mode = true;
    start_radio_bootloader();
}

static void render_bootloader(void) {
    int w = (int)pax_buf_get_width(&fb);
    int h = (int)pax_buf_get_height(&fb);
    pax_background(&fb, COL_BLACK);
    pax_simple_rect(&fb, COL_ACCENT, 0, 0, w, 32);
    pax_draw_text(&fb, COL_BLACK, pax_font_sky_mono, 18, 10, 7, "Radio Bootloader Mode");
    int y = 44;
    pax_draw_text(&fb, COL_GREEN, pax_font_sky_mono, 14, 10, y, "C6 is in bootloader mode."); y += 22;
    pax_draw_text(&fb, COL_WHITE, pax_font_sky_mono, 14, 10, y, "On your Mac:"); y += 20;
    pax_draw_text(&fb, COL_YELLOW, pax_font_sky_mono, 13, 10, y, "ls /dev/cu.usbmodem*"); y += 18;
    pax_draw_text(&fb, COL_GRAY,   pax_font_sky_mono, 13, 10, y, "(find the new C6 USB device)"); y += 22;
    pax_draw_text(&fb, COL_WHITE,  pax_font_sky_mono, 13, 10, y, "cd tanmatsu-radio"); y += 18;
    pax_draw_text(&fb, COL_YELLOW, pax_font_sky_mono, 12, 10, y, "esptool.py --chip esp32c6"); y += 16;
    pax_draw_text(&fb, COL_YELLOW, pax_font_sky_mono, 12, 10, y, "  --port /dev/cu.NEW_DEVICE"); y += 16;
    pax_draw_text(&fb, COL_YELLOW, pax_font_sky_mono, 12, 10, y, "  write_flash 0x0 build/tanmatsu/"); y += 16;
    pax_draw_text(&fb, COL_YELLOW, pax_font_sky_mono, 12, 10, y, "    tanmatsu-radio.bin"); y += 4;
    int fy = h - 26;
    pax_simple_rect(&fb, COL_DARK, 0, fy, w, 26);
    pax_draw_text(&fb, COL_GRAY, pax_font_sky_mono, 14, 8, fy + 6, "ESC/F1: restart badge after flashing");
    blit();
}

static void render(void) {
    if (radio_bootloader_mode) {
        render_bootloader();
        return;
    }
    int w = (int)pax_buf_get_width(&fb);
    int h = (int)pax_buf_get_height(&fb);

    pax_background(&fb, COL_BLACK);

    // Header bar
    pax_simple_rect(&fb, COL_DARK, 0, 0, w, 32);
    pax_draw_text(&fb, COL_ACCENT, pax_font_sky_mono, 18, 10, 7, "MeshCore Settings");

    const char *mode_str = edit_mode ? "[EDIT]" : "[VIEW]";
    pax_col_t   mode_col = edit_mode ? COL_YELLOW : COL_GRAY;
    char hdr_right[32];
    snprintf(hdr_right, sizeof(hdr_right), "%s%s", mode_str, dirty ? " *" : "");
    pax_vec2f sz = pax_text_size(pax_font_sky_mono, 16, hdr_right);
    pax_draw_text(&fb, mode_col, pax_font_sky_mono, 16, w - (int)sz.x - 10, 9, hdr_right);

    // Build display values
    typedef struct { const char *label; char value[64]; } row_t;
    row_t rows[FIELD_COUNT];

    snprintf(rows[FIELD_OWNER].value,   sizeof(rows[FIELD_OWNER].value),   "%s", owner_name);
    rows[FIELD_OWNER].label = "Owner name";

    rows[FIELD_FREQ].label = "Frequency";
    snprintf(rows[FIELD_FREQ].value, sizeof(rows[FIELD_FREQ].value),
             "%.3f MHz", (double)lora_cfg.frequency / 1000000.0);

    rows[FIELD_SF].label = "Spreading factor";
    snprintf(rows[FIELD_SF].value, sizeof(rows[FIELD_SF].value),
             "SF%d", lora_cfg.spreading_factor);

    rows[FIELD_BW].label = "Bandwidth";
    snprintf(rows[FIELD_BW].value, sizeof(rows[FIELD_BW].value),
             "%d kHz", (int)lora_cfg.bandwidth);

    rows[FIELD_CR].label = "Coding rate";
    snprintf(rows[FIELD_CR].value, sizeof(rows[FIELD_CR].value),
             "4/%d", lora_cfg.coding_rate);

    rows[FIELD_POWER].label = "TX power";
    snprintf(rows[FIELD_POWER].value, sizeof(rows[FIELD_POWER].value),
             "%d dBm", (int)lora_cfg.power);

    rows[FIELD_SYNC].label = "Sync word";
    snprintf(rows[FIELD_SYNC].value, sizeof(rows[FIELD_SYNC].value),
             "0x%02X", (unsigned)lora_cfg.sync_word);

    rows[FIELD_PREAMBLE].label = "Preamble length";
    snprintf(rows[FIELD_PREAMBLE].value, sizeof(rows[FIELD_PREAMBLE].value),
             "%d", (int)lora_cfg.preamble_length);

    // Rows
    int row_h = (h - 32 - 28) / FIELD_COUNT;
    int y0    = 34;

    for (int i = 0; i < FIELD_COUNT; i++) {
        int  y      = y0 + i * row_h;
        bool is_sel = (i == selected);

        // Selection background
        if (is_sel) {
            pax_col_t bg = edit_mode ? 0xFF2A1A00 : 0xFF001122;
            pax_simple_rect(&fb, bg, 0, y, w, row_h - 1);
            pax_col_t bar = edit_mode ? COL_YELLOW : COL_ACCENT;
            pax_simple_rect(&fb, bar, 0, y, 4, row_h - 1);
        }

        // Separator
        pax_simple_rect(&fb, COL_DARK, 4, y + row_h - 1, w - 4, 1);

        // Label
        pax_col_t lbl_col = is_sel ? COL_WHITE : COL_GRAY;
        pax_draw_text(&fb, lbl_col, pax_font_sky_mono, 16, 14, y + (row_h - 16) / 2, rows[i].label);

        // Value
        pax_col_t val_col;
        if (i >= FIELD_FREQ && !c6_available) {
            val_col = COL_YELLOW;  // NVS values — C6 not synced
        } else if (is_sel && edit_mode) {
            val_col = COL_YELLOW;
        } else if (is_sel) {
            val_col = COL_WHITE;
        } else {
            val_col = COL_GREEN;
        }

        char val_disp[80];
        if (is_sel && edit_mode && i != FIELD_OWNER) {
            snprintf(val_disp, sizeof(val_disp), "< %s >", rows[i].value);
        } else {
            snprintf(val_disp, sizeof(val_disp), "%s", rows[i].value);
        }
        pax_vec2f vsz = pax_text_size(pax_font_sky_mono, 16, val_disp);
        pax_draw_text(&fb, val_col, pax_font_sky_mono, 16, w - (int)vsz.x - 14, y + (row_h - 16) / 2, val_disp);
    }

    // Footer bar
    int fy = h - 26;
    pax_simple_rect(&fb, COL_DARK, 0, fy, w, 26);
    if (edit_mode && selected != FIELD_OWNER) {
        pax_draw_text(&fb, COL_GRAY, pax_font_sky_mono, 14, 8, fy + 6,
                      "Up/Down or W/S: adjust  Enter: save  ESC/F1: cancel");
    } else if (!c6_available) {
        pax_draw_text(&fb, COL_YELLOW, pax_font_sky_mono, 14, 8, fy + 6,
                      "NVS only - C6 unavailable  U: flash radio  ESC: exit");
    } else {
        pax_draw_text(&fb, COL_GRAY, pax_font_sky_mono, 14, 8, fy + 6,
                      "W/S: navigate  Enter: edit  R: reload  ESC/F1: exit");
    }
    if (dirty) {
        pax_draw_text(&fb, COL_YELLOW, pax_font_sky_mono, 14, w - 110, fy + 6, "* unsaved");
    }

    blit();
}

static void handle_nav(uint32_t key) {
    if (radio_bootloader_mode) {
        if (key == BSP_INPUT_NAVIGATION_KEY_F1 || key == BSP_INPUT_NAVIGATION_KEY_ESC) {
            bsp_led_set_mode(true);
            bsp_device_restart_to_launcher();
        }
        return;
    }
    if (key == BSP_INPUT_NAVIGATION_KEY_F1 || key == BSP_INPUT_NAVIGATION_KEY_ESC) {
        if (edit_mode) {
            edit_mode = false;
            dirty     = false;
            load_owner_name();
            load_lora_config();
        } else {
            bsp_led_set_mode(true);  // Restore automatic LED mode for launcher
            bsp_device_restart_to_launcher();
        }
    } else if (key == BSP_INPUT_NAVIGATION_KEY_UP) {
        if (!edit_mode) {
            selected = (selected - 1 + FIELD_COUNT) % FIELD_COUNT;
        } else if (selected != FIELD_OWNER) {
            field_adjust(selected, +1);
        }
    } else if (key == BSP_INPUT_NAVIGATION_KEY_DOWN) {
        if (!edit_mode) {
            selected = (selected + 1) % FIELD_COUNT;
        } else if (selected != FIELD_OWNER) {
            field_adjust(selected, -1);
        }
    } else if (key == BSP_INPUT_NAVIGATION_KEY_LEFT) {
        if (edit_mode && selected != FIELD_OWNER) {
            field_adjust(selected, -1);
        }
    } else if (key == BSP_INPUT_NAVIGATION_KEY_RIGHT) {
        if (edit_mode && selected != FIELD_OWNER) {
            field_adjust(selected, +1);
        }
    } else if (key == BSP_INPUT_NAVIGATION_KEY_RETURN) {
        if (!edit_mode) {
            edit_mode = true;
        } else {
            if (selected == FIELD_OWNER) {
                save_owner_name();
            } else {
                save_lora_config();
            }
            edit_mode = false;
            dirty     = false;
        }
    }
}

static void handle_key(char c) {
    if (radio_bootloader_mode) {
        if (c == 27) { // ESC
            bsp_led_set_mode(true);
            bsp_device_restart_to_launcher();
        }
        return;
    }
    if (c == 27) { // ESC
        if (edit_mode) {
            edit_mode = false;
            dirty     = false;
            load_owner_name();
            load_lora_config();
        } else {
            bsp_led_set_mode(true);  // Restore automatic LED mode for launcher
            bsp_device_restart_to_launcher();
        }
    } else if (c == 'w' || c == 'W') {
        if (!edit_mode) selected = (selected - 1 + FIELD_COUNT) % FIELD_COUNT;
        else if (selected != FIELD_OWNER) field_adjust(selected, +1);
    } else if (c == 's' || c == 'S') {
        if (!edit_mode) selected = (selected + 1) % FIELD_COUNT;
        else if (selected != FIELD_OWNER) field_adjust(selected, -1);
    } else if (c == '<' || c == ',') {
        if (edit_mode && selected != FIELD_OWNER) field_adjust(selected, -1);
    } else if (c == '>' || c == '.') {
        if (edit_mode && selected != FIELD_OWNER) field_adjust(selected, +1);
    } else if (c == '\r' || c == '\n') {
        if (!edit_mode) {
            edit_mode = true;
        } else {
            if (selected == FIELD_OWNER) save_owner_name();
            else save_lora_config();
            edit_mode = false;
            dirty     = false;
        }
    } else if (c == 'r' || c == 'R') {
        load_owner_name();
        load_lora_config();
        dirty     = false;
        edit_mode = false;
    } else if ((c == 'u' || c == 'U') && !c6_available && !edit_mode) {
        enter_radio_bootloader();
    }
}

void app_main(void) {
    gpio_install_isr_service(0);

    // NVS
    esp_err_t res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES || res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        res = nvs_flash_init();
    }
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %d", res);
        return;
    }

    // BSP
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

    // Display
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

    // Helper to print a diagnostic line on the loading screen
    int    diag_y    = 40;
    int    diag_line = 20;
#define DIAG(col, fmt, ...) do { \
        char _buf[80]; \
        snprintf(_buf, sizeof(_buf), fmt, ##__VA_ARGS__); \
        ESP_LOGI(TAG, "%s", _buf); \
        pax_draw_text(&fb, (col), pax_font_sky_mono, 14, 10, diag_y, _buf); \
        diag_y += diag_line; \
        blit(); \
    } while(0)

    pax_background(&fb, COL_RED);
    pax_draw_text(&fb, COL_WHITE, pax_font_sky_mono, 18, 10, 10, "MeshCore Settings v3");
    blit();
    vTaskDelay(pdMS_TO_TICKS(2000));

    DIAG(COL_GRAY, "wifi_connection_init_stack...");
    res = wifi_connection_init_stack();
    DIAG(res == ESP_OK ? COL_GREEN : COL_RED, "  wifi init: %s (%d)",
         res == ESP_OK ? "OK" : "FAIL", res);
    if (res == ESP_OK) {
        DIAG(COL_GRAY, "wifi_connect_try_all...");
        res = wifi_connect_try_all();
        DIAG(res == ESP_OK ? COL_GREEN : COL_YELLOW, "  wifi connect: %s (%d)",
             res == ESP_OK ? "OK" : "no saved networks", res);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));

    load_owner_name();

    DIAG(COL_GRAY, "lora_init...");
    res = lora_init(4);
    DIAG(res == ESP_OK ? COL_GREEN : COL_RED, "  lora_init: %s (%d)",
         res == ESP_OK ? "OK" : "FAIL", res);

    load_lora_from_nvs();
    lora_ready = true;
    DIAG(COL_GREEN, "NVS: freq=%.3fMHz SF%d BW%d",
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
                DIAG(COL_GREEN, "  C6 OK! freq=%.3fMHz SF%d",
                     (double)lora_cfg.frequency / 1000000.0, lora_cfg.spreading_factor);
            } else {
                DIAG(COL_YELLOW, "  C6 fresh - pushing NVS config");
                lora_set_config(&lora_cfg);
            }
        } else {
            DIAG(COL_YELLOW, "  C6 unavail (err=%d) - using NVS", cfg_res);
        }
    } else {
        DIAG(COL_YELLOW, "lora_init failed - using NVS values");
    }

    // Pause so user can read the diagnostics
    vTaskDelay(pdMS_TO_TICKS(4000));
#undef DIAG

    render();

    // Main event loop
    while (1) {
        bsp_input_event_t event;
        if (xQueueReceive(input_event_queue, &event, portMAX_DELAY) != pdTRUE) continue;

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
