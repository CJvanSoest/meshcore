// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "render.h"
#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "bsp/display.h"
#include "bsp/power.h"
#include "channels.h"
#include "chat.h"
#include "contacts.h"
#include "emoji.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"
#include "radio.h"
#include "render_internal.h"
#include "ui_state.h"

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

// Shared Pager-style header for the four classic views: view name (+ DM /
// channel unread badges) on the left, RX | TX (duty cycle %) | battery on
// the right. Replaces the original coloured tab-bar so the home screen and
// the classic views share one visual identity.
void render_tab_bar(void) {
    int                w                          = (int)pax_buf_get_width(&fb);
    static const char* tab_labels[VIEW_TAB_COUNT] = {"Settings", "Nodes", "DM", "Channel"};

    pax_simple_rect(&fb, COL_PAGER_BG, 0, 0, w, TAB_BAR_H);
    pax_simple_rect(&fb, COL_PAGER_ACCENT, 0, TAB_BAR_H - 1, w, 1);

    int label_y = (TAB_BAR_H - TXT_TAB) / 2;
    int x       = 12;

    // Left: view name in Pager text colour. Falls back gracefully if
    // current_view is out of range (shouldn't happen, but be safe).
    const char* vname = (current_view >= 0 && current_view < VIEW_TAB_COUNT) ? tab_labels[current_view] : "";
    if (vname[0]) {
        pax_vec2f vsz = pax_text_size(FONT, TXT_TAB, vname);
        pax_draw_text(&fb, COL_PAGER_TEXT, FONT, TXT_TAB, x, label_y, vname);
        x += (int)vsz.x + 12;
    }

    // Inline unread badges for the *other* conversation tabs — so a DM or
    // channel message arriving while you're in Settings is still visible.
    int badge_y   = (TAB_BAR_H - TXT_SMALL) / 2 - 2;
    int badge_h   = TXT_SMALL + 4;
    int dm_unread = contact_unread_total();
    if (dm_unread > 0 && current_view != VIEW_CHAT) {
        char buf[8];
        snprintf(buf, sizeof(buf), "DM %d", dm_unread > 99 ? 99 : dm_unread);
        pax_vec2f sz = pax_text_size(FONT, TXT_SMALL, buf);
        int       bw = (int)sz.x + 12;
        pax_simple_rect(&fb, COL_RED, x, badge_y, bw, badge_h);
        pax_draw_text(&fb, COL_PAGER_BG, FONT, TXT_SMALL, x + 6, badge_y + 2, buf);
        x += bw + 8;
    }
    int ch_unread = channel_unread_total();
    if (ch_unread > 0 && current_view != VIEW_CHANNEL) {
        char buf[8];
        snprintf(buf, sizeof(buf), "# %d", ch_unread > 99 ? 99 : ch_unread);
        pax_vec2f sz = pax_text_size(FONT, TXT_SMALL, buf);
        int       bw = (int)sz.x + 12;
        pax_simple_rect(&fb, COL_RED, x, badge_y, bw, badge_h);
        pax_draw_text(&fb, COL_PAGER_BG, FONT, TXT_SMALL, x + 6, badge_y + 2, buf);
        x += bw + 8;
    }

    // Right: RX | TX (duty cycle %) | battery — same layout as home header.
    int status_x = w - 12;
    int status_y = (TAB_BAR_H - TXT_BODY) / 2;

    bsp_power_battery_information_t bat = {0};
    if (bsp_power_get_battery_information(&bat) == ESP_OK && bat.battery_available) {
        int pct = (int)bat.remaining_percentage;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        char buf[16];
        snprintf(buf, sizeof(buf), "%d%%%s", pct, bat.battery_charging ? "+" : "");
        pax_col_t col  = pct <= 20 ? COL_RED : (pct <= 50 ? COL_AMBER : COL_GREEN);
        pax_vec2f sz   = pax_text_size(FONT, TXT_BODY, buf);
        status_x      -= (int)sz.x;
        pax_draw_text(&fb, col, FONT, TXT_BODY, status_x, status_y, buf);
        status_x -= 14;
    }

    if (dc_budget_ms > 0 && dc_budget_ms < 3600000u) {
        unsigned pct_x10 = (unsigned)(((uint64_t)dc_used_ms * 1000u) / dc_budget_ms);
        char     buf[16];
        snprintf(buf, sizeof(buf), "TX:%u.%u%%", pct_x10 / 10u, pct_x10 % 10u);
        pax_col_t col  = dc_last_tx_blocked ? COL_RED : (pct_x10 >= 800) ? COL_AMBER : COL_PAGER_TEXT;
        pax_vec2f sz   = pax_text_size(FONT, TXT_BODY, buf);
        status_x      -= (int)sz.x;
        pax_draw_text(&fb, col, FONT, TXT_BODY, status_x, status_y, buf);
        status_x -= 14;
    }

    if (lora_rx_ok) {
        int cnt = 0;
        if (xSemaphoreTake(rx_mutex, 0) == pdTRUE) {
            cnt = rx_count;
            xSemaphoreGive(rx_mutex);
        }
        char buf[12];
        snprintf(buf, sizeof(buf), "RX:%d", cnt);
        pax_vec2f sz  = pax_text_size(FONT, TXT_BODY, buf);
        status_x     -= (int)sz.x;
        pax_draw_text(&fb, COL_GREEN, FONT, TXT_BODY, status_x, status_y, buf);
    }
}

// 2x4 emoji picker overlay. Drawn on top of an already-rendered chat view.
// Active state owned by chat module.
void render_emoji_picker_overlay(void) {
    int w = (int)pax_buf_get_width(&fb);
    int h = (int)pax_buf_get_height(&fb);

    const int rows    = 2;
    const int cols    = 4;
    const int cell    = 52;
    const int pad     = 14;
    const int panel_w = cols * cell + 2 * pad;
    const int panel_h = rows * cell + 2 * pad + TXT_SMALL + 6;
    int       panel_x = (w - panel_w) / 2;
    int       panel_y = h - CHAT_INPUT_H - FOOTER_H - panel_h - 4;
    if (panel_y < TAB_BAR_H + 4) panel_y = TAB_BAR_H + 4;

    pax_simple_rect(&fb, COL_HEADER, panel_x, panel_y, panel_w, panel_h);
    pax_simple_rect(&fb, COL_ACCENT, panel_x, panel_y, panel_w, 2);
    pax_draw_text(&fb, COL_AMBER, FONT, TXT_SMALL, panel_x + pad, panel_y + 4, "Pick emoji");

    int grid_x = panel_x + pad;
    int grid_y = panel_y + 6 + TXT_SMALL;

    for (int i = 0; i < EMOJI_COUNT; i++) {
        int  r   = i / cols;
        int  c   = i % cols;
        int  cx  = grid_x + c * cell + cell / 2;
        int  cy  = grid_y + r * cell + cell / 2;
        bool sel = (i == emoji_picker_cursor);
        if (sel) {
            pax_simple_rect(&fb, COL_PANEL, cx - cell / 2 + 2, cy - cell / 2 + 2, cell - 4, cell - 4);
        }
        emoji_draw(i, cx, cy, cell / 2 - 6, &fb);
    }
}

void render(void) {
    // Single-flush model: each render_*() draws into fb but does NOT blit.
    // Overlays (QR, emoji picker) draw on top of the base view, and we blit
    // exactly once at the end so the user never sees the base layer briefly
    // through an overlay swap (the old double-blit caused QR/emoji flicker).
    switch (current_view) {
        case VIEW_HOME:
            render_home();
            break;
        case VIEW_ABOUT:
            render_about();
            break;
        case VIEW_MAP:
            render_map();
            break;
        case VIEW_TOOLBOX:
            render_toolbox();
            break;
        case VIEW_TOOLBOX_LOG:
            render_toolbox_log();
            break;
        case VIEW_NODES:
            render_nodes();
            if (qr_overlay_active) render_qr_overlay();
            break;
        case VIEW_CHAT:
            render_chat();
            break;
        case VIEW_CHANNEL:
            render_channel();
            break;
        case VIEW_SETTINGS:
        default:
            render_settings();
            if (qr_overlay_active) render_qr_overlay();
            break;
    }
    if (emoji_picker_active && chat_typing && (current_view == VIEW_CHAT || current_view == VIEW_CHANNEL)) {
        render_emoji_picker_overlay();
    }
    blit();
}
