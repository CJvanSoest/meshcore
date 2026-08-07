// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// LVGL UI entry point: lifecycle and the per-view dispatch. The views
// themselves live in lvgl_<view>.c; shared drawing helpers are in
// lvgl_common.c behind lvgl_internal.h.

#include "lvgl_ui.h"
#include "lvgl_internal.h"
#include "lvgl_port.h"
#include "map.h"
#include "mc_fonts.h"  // extended Montserrat faces (umlauts, accents, °, €, …)
#include "nodes.h"
#include "nvs_flash.h"
#include "qrcodegen.h"
#include "radio.h"
#include "region_limits.h"
#include "render.h"  // COL_* palette + TXT_* sizes
#include "render_internal.h"
#include "settings_nvs.h"
#include "special_table.h"  // special_font_covers (draw umlauts instead of '?')
#include "ui_state.h"
#include "wifi_connection.h"

// ── Dispatch ─────────────────────────────────────────────────────────────────

bool lvgl_view_active(app_view_t v) {
    switch (v) {
        case VIEW_ABOUT:
        case VIEW_HOME:
        case VIEW_MAP:
        case VIEW_TOOLBOX:
        case VIEW_TOOLBOX_COVERAGE:
        case VIEW_TOOLBOX_LOG:
        case VIEW_TOOLBOX_STORAGE:
        case VIEW_BLE_DEVICES:
            return true;
        case VIEW_SETTINGS:
            return true;
        case VIEW_NODES:
            // Nodes + its QR overlay both render through LVGL now.
            return true;
        case VIEW_CHAT:
            // Chat + the emoji-picker overlay both render through LVGL now.
            return true;
        case VIEW_CHANNEL:
            // Channel + its QR (share) overlay both render through LVGL now.
            return true;
        default:
            return false;
    }
}

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
