// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "render.h"
#include "app_config.h"
#include "bsp/display.h"
#include "esp_log.h"
#include "lvgl_ui.h"
#include "pax_gfx.h"

static const char* TAG = "render";

size_t    display_h_res = 0;
size_t    display_v_res = 0;
pax_buf_t fb            = {0};

void blit(void) {
    esp_err_t res = bsp_display_blit(0, 0, display_h_res, display_v_res, pax_buf_get_pixels(&fb));
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "blit failed: %d", res);
    }
}

void render(void) {
    // LVGL-only: every view renders through LVGL (its flush_cb owns the panel).
    lvgl_view_render(current_view);
}
