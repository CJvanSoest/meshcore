// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "render.h"
#include "render_internal.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "bsp/display.h"
#include "bsp/power.h"
#include "esp_log.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"

#include "app_config.h"
#include "channels.h"
#include "chat.h"
#include "contacts.h"
#include "emoji.h"
#include "radio.h"
#include "ui_state.h"

static const char *TAG = "render";

size_t    display_h_res = 0;
size_t    display_v_res = 0;
pax_buf_t fb            = {0};

void blit(void) {
    esp_err_t res = bsp_display_blit(0, 0, display_h_res, display_v_res, pax_buf_get_pixels(&fb));
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "blit failed: %d", res);
    }
}

void render_tab_bar(void) {
    int w = (int)pax_buf_get_width(&fb);
    // Tab labels in app_view_t enum order — only the four classic views
    // appear here; VIEW_HOME has its own header in render_home.c.
    static const char *tab_labels[VIEW_TAB_COUNT] = {"Settings", "Nodes", "DM", "Channel"};
    int tab_w = (w - 200) / VIEW_TAB_COUNT;

    pax_simple_rect(&fb, COL_HEADER, 0, 0, w, TAB_BAR_H);

    int label_y = (TAB_BAR_H - TXT_TAB) / 2;
    for (int i = 0; i < VIEW_TAB_COUNT; i++) {
        bool active = (i == (int)current_view);
        if (active) {
            pax_simple_rect(&fb, COL_ACCENT, i * tab_w, 0, tab_w, TAB_BAR_H);
        }
        pax_col_t col = active ? COL_HEADER : COL_GRAY;
        pax_vec2f sz  = pax_text_size(FONT, TXT_TAB, tab_labels[i]);

        int unread = (i == VIEW_CHAT) ? contact_unread_total()
                   : (i == VIEW_CHANNEL) ? channel_unread_total() : 0;
        int badge_w = 0;
        char badge_txt[8] = {0};
        if (unread > 0) {
            snprintf(badge_txt, sizeof(badge_txt), "%d", unread > 99 ? 99 : unread);
            pax_vec2f bsz = pax_text_size(FONT, TXT_SMALL, badge_txt);
            badge_w = (int)bsz.x + 12;
        }

        int total_w = (int)sz.x + (badge_w ? badge_w + 6 : 0);
        int tx      = i * tab_w + (tab_w - total_w) / 2;
        pax_draw_text(&fb, col, FONT, TXT_TAB, tx, label_y, tab_labels[i]);

        if (badge_w) {
            int bx = tx + (int)sz.x + 6;
            int by = (TAB_BAR_H - TXT_SMALL) / 2 - 2;
            int bh = TXT_SMALL + 4;
            pax_simple_rect(&fb, COL_RED, bx, by, badge_w, bh);
            pax_vec2f tsz = pax_text_size(FONT, TXT_SMALL, badge_txt);
            int label_tx = bx + (badge_w - (int)tsz.x) / 2;
            pax_draw_text(&fb, COL_HEADER, FONT, TXT_SMALL, label_tx, by + 2, badge_txt);
        }
    }
    pax_simple_rect(&fb, COL_PANEL, 0, TAB_BAR_H - 1, w, 1);

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
    pax_draw_text(&fb, COL_AMBER, FONT, TXT_SMALL,
                  panel_x + pad, panel_y + 4, "Pick emoji");

    int grid_x = panel_x + pad;
    int grid_y = panel_y + 6 + TXT_SMALL;

    for (int i = 0; i < EMOJI_COUNT; i++) {
        int r  = i / cols;
        int c  = i % cols;
        int cx = grid_x + c * cell + cell / 2;
        int cy = grid_y + r * cell + cell / 2;
        bool sel = (i == emoji_picker_cursor);
        if (sel) {
            pax_simple_rect(&fb, COL_PANEL,
                            cx - cell / 2 + 2, cy - cell / 2 + 2,
                            cell - 4, cell - 4);
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
        case VIEW_HOME:    render_home();    break;
        case VIEW_NODES:
            render_nodes();
            if (qr_overlay_active) render_qr_overlay();
            break;
        case VIEW_CHAT:    render_chat();    break;
        case VIEW_CHANNEL: render_channel(); break;
        case VIEW_SETTINGS:
        default:           render_settings(); break;
    }
    if (emoji_picker_active && chat_typing &&
        (current_view == VIEW_CHAT || current_view == VIEW_CHANNEL)) {
        render_emoji_picker_overlay();
    }
    blit();
}
