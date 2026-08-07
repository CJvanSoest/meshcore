// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// LVGL UI entry point: lifecycle and the per-view dispatch. The views
// themselves live in lvgl_<view>.c; shared drawing helpers are in
// lvgl_common.c behind lvgl_internal.h.

#include "app_config.h"
#include "history.h"
#include "lvgl.h"
#include "lvgl_internal.h"
#include "lvgl_port.h"
#include "render.h"
#include "ui_state.h"

// ── Dispatch ─────────────────────────────────────────────────────────────────

void lvgl_view_render(app_view_t v) {
    switch (v) {
        case VIEW_SETTINGS:
            render_settings_lvgl();
            break;
        case VIEW_ABOUT:
            render_about_lvgl();
            break;
        case VIEW_HOME:
            render_home_lvgl();
            break;
        case VIEW_MAP:
            render_map_lvgl();
            break;
        case VIEW_TOOLBOX:
            render_toolbox_lvgl();
            break;
        case VIEW_TOOLBOX_COVERAGE:
            render_toolbox_coverage_lvgl();
            break;
        case VIEW_TOOLBOX_LOG:
            render_toolbox_log_lvgl();
            break;
        case VIEW_TOOLBOX_STORAGE:
            render_toolbox_storage_lvgl();
            break;
        case VIEW_BLE_DEVICES:
            render_ble_devices_lvgl();
            break;
        case VIEW_NODES:
            if (qr_overlay_active) {
                render_qr_overlay_lvgl();
            } else {
                render_nodes_lvgl();
            }
            break;
        case VIEW_CHAT:
            render_chat_lvgl();
            break;
        case VIEW_CHANNEL:
            if (qr_overlay_active) {
                render_qr_overlay_lvgl();
            } else {
                render_channel_lvgl();
            }
            break;
        default:
            return;
    }
    lvgl_port_refresh_now();
}
