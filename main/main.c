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

// App state
static int                           selected   = 0;
static bool                          edit_mode  = false;
static bool                          dirty      = false;
static bool                          lora_ready = false;
static char                          owner_name[33] = {0};
static lora_protocol_config_params_t lora_cfg   = {0};

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

static void load_lora_config(void) {
    esp_err_t res = lora_get_config(&lora_cfg);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "lora_get_config failed: %d", res);
        lora_ready = false;
    } else {
        lora_ready = true;
    }
}

static void save_lora_config(void) {
    esp_err_t res = lora_set_config(&lora_cfg);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "lora_set_config failed: %d", res);
    } else {
        ESP_LOGI(TAG, "LoRa config saved");
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

static void render(void) {
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

    if (lora_ready) {
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
    } else {
        rows[FIELD_FREQ].label     = "Frequency";
        rows[FIELD_SF].label       = "Spreading factor";
        rows[FIELD_BW].label       = "Bandwidth";
        rows[FIELD_CR].label       = "Coding rate";
        rows[FIELD_POWER].label    = "TX power";
        rows[FIELD_SYNC].label     = "Sync word";
        rows[FIELD_PREAMBLE].label = "Preamble length";
        for (int i = FIELD_FREQ; i < FIELD_COUNT; i++) {
            snprintf(rows[i].value, sizeof(rows[i].value), "(LoRa unavailable)");
        }
    }

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
        if (i >= FIELD_FREQ && !lora_ready) {
            val_col = COL_RED;
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
    if (key == BSP_INPUT_NAVIGATION_KEY_F1 || key == BSP_INPUT_NAVIGATION_KEY_ESC) {
        if (edit_mode) {
            edit_mode = false;
            dirty     = false;
            load_owner_name();
            if (lora_ready) load_lora_config();
        } else {
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
            } else if (lora_ready) {
                save_lora_config();
            }
            edit_mode = false;
            dirty     = false;
        }
    }
}

static void handle_key(char c) {
    if (c == 27) { // ESC
        if (edit_mode) {
            edit_mode = false;
            dirty     = false;
            load_owner_name();
            if (lora_ready) load_lora_config();
        } else {
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
            else if (lora_ready) save_lora_config();
            edit_mode = false;
            dirty     = false;
        }
    } else if (c == 'r' || c == 'R') {
        load_owner_name();
        if (lora_ready) load_lora_config();
        dirty     = false;
        edit_mode = false;
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
    bsp_led_set_mode(false);

    // Show loading screen
    pax_background(&fb, COL_BLACK);
    pax_draw_text(&fb, COL_ACCENT, pax_font_sky_mono, 18, 10, 10, "MeshCore Settings");
    pax_draw_text(&fb, COL_GRAY,   pax_font_sky_mono, 16, 10, 40, "Loading settings...");
    blit();

    // Load owner name from NVS
    load_owner_name();

    // Initialize LoRa and load config
    res = lora_init(4);
    if (res == ESP_OK) {
        load_lora_config();
    } else {
        ESP_LOGW(TAG, "LoRa init failed (%d), LoRa settings unavailable", res);
        lora_ready = false;
    }

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
