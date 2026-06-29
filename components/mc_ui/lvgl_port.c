// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "lvgl_port.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

static const char* TAG = "lvgl_port";

// Native panel geometry (the buffer bsp_display_blit consumes).
static size_t s_native_w = 0;
static size_t s_native_h = 0;
// Logical geometry LVGL draws in (rotation-corrected).
static size_t s_log_w    = 0;
static size_t s_log_h    = 0;

static bsp_display_rotation_t s_rot    = BSP_DISPLAY_ROTATION_0;
static bool                   s_swap16 = false;  // big-endian panel byte swap
static bool                   s_ready  = false;

static lv_display_t* s_disp    = NULL;
static lv_obj_t*     s_screen  = NULL;
static uint16_t*     s_log_buf = NULL;  // LVGL full-frame draw buffer (logical)
static uint16_t*     s_native  = NULL;  // rotated scratch handed to the panel

static esp_timer_handle_t s_tick_timer = NULL;

// 1 ms LVGL tick from a periodic esp_timer. Only touches LVGL's tick counter,
// which is safe from timer-task context.
static void tick_cb(void* arg) {
    (void)arg;
    lv_tick_inc(1);
}

// Map a logical pixel (lx, ly) to its index in the native (pre-rotation) buffer.
// The four cases mirror exactly what main.c selects for the PAX framebuffer:
//   ROTATION_0   -> upright
//   ROTATION_90  -> PAX_O_ROT_CCW  (ccw1: {ly, nh-1-lx})
//   ROTATION_180 -> PAX_O_ROT_HALF (ccw2: {nw-1-lx, nh-1-ly})
//   ROTATION_270 -> PAX_O_ROT_CW   (ccw3: {nw-1-ly, lx})   <-- Tanmatsu default
// so the LVGL output lands in the same orientation as the known-good PAX path.
static inline size_t native_index(int lx, int ly) {
    int nw = (int)s_native_w;
    int nh = (int)s_native_h;
    switch (s_rot) {
        case BSP_DISPLAY_ROTATION_90: {
            int px = ly;
            int py = nh - 1 - lx;
            return (size_t)py * nw + px;
        }
        case BSP_DISPLAY_ROTATION_180: {
            int px = nw - 1 - lx;
            int py = nh - 1 - ly;
            return (size_t)py * nw + px;
        }
        case BSP_DISPLAY_ROTATION_270: {
            int px = nw - 1 - ly;
            int py = lx;
            return (size_t)py * nw + px;
        }
        case BSP_DISPLAY_ROTATION_0:
        default:
            return (size_t)ly * nw + lx;
    }
}

static void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    const uint16_t* src    = (const uint16_t*)px_map;
    int             area_w = area->x2 - area->x1 + 1;
    for (int y = area->y1; y <= area->y2; y++) {
        for (int x = area->x1; x <= area->x2; x++) {
            uint16_t p = src[(y - area->y1) * area_w + (x - area->x1)];
            if (s_swap16) {
                p = (uint16_t)((p << 8) | (p >> 8));
            }
            s_native[native_index(x, y)] = p;
        }
    }
    // Full-refresh render mode flushes the whole logical screen in one call, so
    // the rotated scratch now holds a complete native frame: blit it all.
    esp_err_t res = bsp_display_blit(0, 0, s_native_w, s_native_h, s_native);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "blit failed: %d", res);
    }
    lv_display_flush_ready(disp);
}

esp_err_t lvgl_port_init(size_t native_w, size_t native_h, bsp_display_color_format_t fmt,
                         bsp_display_endianness_t endian, bsp_display_rotation_t rot) {
    if (s_ready) {
        return ESP_OK;
    }
    s_native_w = native_w;
    s_native_h = native_h;
    s_rot      = rot;
    s_swap16   = (endian == BSP_DISPLAY_ENDIAN_BIG);

    // Odd rotations swap width/height for the logical surface.
    if (rot == BSP_DISPLAY_ROTATION_90 || rot == BSP_DISPLAY_ROTATION_270) {
        s_log_w = native_h;
        s_log_h = native_w;
    } else {
        s_log_w = native_w;
        s_log_h = native_h;
    }

    if (fmt != BSP_DISPLAY_COLOR_FORMAT_16_565RGB) {
        // The LVGL build is configured for 16-bit colour; a different panel
        // format would need a conversion path we don't have yet.
        ESP_LOGW(TAG, "panel format %d != 565RGB; LVGL assumes 16-bit", (int)fmt);
    }

    size_t px = s_log_w * s_log_h;
    s_log_buf = heap_caps_malloc(px * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    s_native  = heap_caps_malloc((size_t)s_native_w * s_native_h * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!s_log_buf || !s_native) {
        ESP_LOGE(TAG, "draw buffer alloc failed (%zu px)", px);
        return ESP_ERR_NO_MEM;
    }

    lv_init();

    s_disp = lv_display_create((int32_t)s_log_w, (int32_t)s_log_h);
    if (!s_disp) {
        ESP_LOGE(TAG, "lv_display_create failed");
        return ESP_FAIL;
    }
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_set_buffers(s_disp, s_log_buf, NULL, px * sizeof(uint16_t), LV_DISPLAY_RENDER_MODE_FULL);

    s_screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_size(s_screen, (int32_t)s_log_w, (int32_t)s_log_h);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_screen_load(s_screen);

    const esp_timer_create_args_t targs = {
        .callback = tick_cb,
        .name     = "lv_tick",
    };
    if (esp_timer_create(&targs, &s_tick_timer) == ESP_OK) {
        esp_timer_start_periodic(s_tick_timer, 1000);  // 1 ms
    } else {
        ESP_LOGW(TAG, "lv_tick timer create failed");
    }

    s_ready = true;
    ESP_LOGI(TAG, "LVGL up: logical %zux%zu, native %zux%zu, rot %d", s_log_w, s_log_h, s_native_w, s_native_h,
             (int)rot);
    return ESP_OK;
}

bool lvgl_port_ready(void) {
    return s_ready;
}

size_t lvgl_port_width(void) {
    return s_log_w;
}

size_t lvgl_port_height(void) {
    return s_log_h;
}

void lvgl_port_refresh_now(void) {
    if (!s_ready) {
        return;
    }
    lv_refr_now(s_disp);
}

void* lvgl_port_screen(void) {
    return s_screen;
}
