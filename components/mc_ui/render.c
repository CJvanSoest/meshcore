// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "render.h"
#include "app_config.h"  // current_view
#include "lvgl_ui.h"

// Native panel geometry, filled in app_main from bsp_display_get_parameters and
// handed to lvgl_port_init. Kept here (declared in render.h) so main.c and the
// LVGL glue share one definition.
size_t display_h_res = 0;
size_t display_v_res = 0;

void render(void) {
    // LVGL-only: every view renders through LVGL (its flush_cb owns the panel).
    lvgl_view_render(current_view);
}
