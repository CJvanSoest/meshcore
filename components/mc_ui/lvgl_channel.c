// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// VIEW_CHANNEL.

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "app_config.h"
#include "appfs.h"
#include "backup.h"
#include "ble_companion.h"
#include "bsp/power.h"
#include "channels.h"
#include "chat.h"
#include "contacts.h"
#include "coverage.h"
#include "diag.h"
#include "diag_decode.h"
#include "emoji_table.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gps_task.h"
#include "history.h"
#include "identity.h"
#include "locfs.h"
#include "lvgl.h"
#include "lvgl_internal.h"
#include "lvgl_port.h"
#include "lvgl_ui.h"
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

// ── VIEW_CHANNEL (channel list + wizard + conversation) ──────────────────────
// Pixel-matched port of render_channel.c. The QR (share) overlay and the
// emoji-picker overlay stay on the PAX path (lvgl_view_active reports false
// while either is up).

static void render_channel_list_lvgl(lv_obj_t* scr, int w, int h) {
    const int row_h    = 38;
    const int footer_h = FOOTER_H;

    add_rect(scr, 0, CHAT_Y0, w, 28, COL_PANEL);
    add_label(scr, 10, CHAT_Y0 + 4, TXT_BODY, COL_WHITE, "Channels");

    int rows_y0  = CHAT_Y0 + 32;
    int rows_h   = h - rows_y0 - footer_h;
    int rows_vis = rows_h / row_h;
    if (rows_vis < 1) rows_vis = 1;

    if (channel_list_cursor < 0) channel_list_cursor = 0;
    if (channel_list_cursor >= channel_count) channel_list_cursor = channel_count - 1;

    int scroll = 0;
    if (channel_list_cursor >= rows_vis) scroll = channel_list_cursor - rows_vis + 1;

    for (int row = 0; row < rows_vis && (row + scroll) < channel_count; row++) {
        int  i         = row + scroll;
        int  y         = rows_y0 + row * row_h;
        bool is_sel    = (i == channel_list_cursor);
        bool is_active = (i == active_channel_idx);

        if (is_sel) {
            add_rect(scr, 0, y, w, row_h - 1, COL_PANEL);
            add_rect(scr, 0, y, 5, row_h - 1, COL_ACCENT);
        }
        uint32_t name_col = is_sel ? COL_WHITE : COL_GRAY;
        int      text_y   = y + (row_h - TXT_BODY) / 2;

        if (is_active) {
            add_label(scr, 18, text_y, TXT_BODY, COL_GREEN, ">");
        }
        add_label(scr, 40, text_y, TXT_BODY, name_col, channels[i].name);

        if (channel_unread[i] > 0) {
            char ub[8];
            snprintf(ub, sizeof(ub), "%d", channel_unread[i] > 99 ? 99 : channel_unread[i]);
            int nw = text_w(channels[i].name, TXT_BODY);
            int bw = text_w(ub, TXT_SMALL) + 12;
            int bx = 40 + nw + 8;
            int by = y + (row_h - (TXT_SMALL + 4)) / 2;
            add_rect(scr, bx, by, bw, TXT_SMALL + 4, COL_RED);
            add_label(scr, bx + 6, by + 2, TXT_SMALL, COL_HEADER, ub);
        }

        char meta[24];
        snprintf(meta, sizeof(meta), "0x%02X", channels[i].hash);
        add_label(scr, w - text_w(meta, TXT_BODY) - 14, y + (row_h - TXT_BODY) / 2, TXT_BODY, name_col, meta);
    }

    if (channel_adding && channel_wiz_step == 0) {
        const char* opts[2] = {"# community channel", "private channel"};
        const char* mtitle  = channel_creating ? "Create channel" : "Add channel";
        const int   mpw     = 320;
        const int   mph     = 34 + 2 * 40 + 12;
        int         mpx     = (w - mpw) / 2;
        int         mpy     = (h - mph) / 2;
        add_rect(scr, mpx, mpy, mpw, mph, COL_HEADER);
        add_rect(scr, mpx, mpy, mpw, 2, COL_ACCENT);
        add_label(scr, mpx + 14, mpy + 8, TXT_SMALL, COL_AMBER, mtitle);
        for (int i = 0; i < 2; i++) {
            int oy = mpy + 34 + i * 40;
            if (i == channel_wiz_cursor) {
                add_rect(scr, mpx + 6, oy - 4, mpw - 12, 34, COL_PANEL);
                add_rect(scr, mpx + 6, oy - 4, 4, 34, COL_ACCENT);
            }
            add_label(scr, mpx + 18, oy, TXT_BODY, COL_WHITE, opts[i]);
        }
    } else if (channel_adding) {
        int iy = h - CHAT_INPUT_H - footer_h;
        add_rect(scr, 0, iy, w, CHAT_INPUT_H, COL_PANEL);
        add_rect(scr, 0, iy, w, 2, COL_ACCENT);
        const char* prefix = (channel_wiz_step == 2) ? "secret: " : "name: ";
        const char* shown  = field_edit_buf;
        const int   window = 36;
        if (field_edit_len > window) shown = field_edit_buf + (field_edit_len - window);
        char disp[160];
        snprintf(disp, sizeof(disp), "%s%s_", prefix, shown);
        add_label(scr, 10, iy + (CHAT_INPUT_H - TXT_BODY) / 2, TXT_BODY, COL_WHITE, disp);
    }

    int fy = h - footer_h;
    add_rect(scr, 0, fy, w, footer_h, COL_HEADER);
    add_rect(scr, 0, fy, w, 1, COL_PANEL);
    const char* hint;
    if (channel_adding) {
        if (channel_wiz_step == 0) {
            hint = "W/S: choose   Enter: select   ";
        } else if (channel_wiz_step == 2) {
            hint = "Type the 32-hex secret key   Enter: add   ";
        } else if (channel_wiz_private && !channel_creating) {
            hint = "Type the channel name   Enter: next (secret)   ";
        } else {
            hint = channel_creating ? "Type the channel name   Enter: create   "
                                    : "Type the # channel name (no #)   Enter: add   ";
        }
    } else if (channel_list_cursor == 0) {
        hint = "W/S: nav  Enter: open  A: add  C: create  Q: share  Tab: next  ";
    } else {
        hint = "W/S: nav  Enter: open  A: add  C: create  Q: share  D: del  Tab: next  ";
    }
    int hint_ty = fy + (footer_h - TXT_SMALL) / 2;
    add_label(scr, 10, hint_ty, TXT_SMALL, COL_HINT, hint);
    add_back_hint(scr, 10 + text_w(hint, TXT_SMALL), hint_ty, channel_adding ? ": cancel" : ": home", TXT_SMALL);
    // Channel-slot usage (active channels incl. Public / capacity). Bottom-right,
    // shown as soon as the channel tile opens (hidden during the add wizard).
    if (!channel_adding) {
        char chcc[12];
        snprintf(chcc, sizeof(chcc), "%d/%d", channel_count, CHANNELS_MAX);
        add_label(scr, w - text_w(chcc, TXT_SMALL) - 10, hint_ty, TXT_SMALL, COL_GRAY, chcc);
    }
}

void render_channel_lvgl(void) {
    int       w   = (int)lvgl_port_width();
    int       h   = (int)lvgl_port_height();
    lv_obj_t* scr = begin_screen(COL_BG);
    pt_reset();
    tab_bar_lvgl(scr);

    if (channel_list_mode) {
        render_channel_list_lvgl(scr, w, h);
        return;
    }

    const int hdr_h = 50;
    add_rect(scr, 0, CHAT_Y0, w, hdr_h, COL_PANEL);
    {
        const char* nm = (active_channel_idx >= 0 && active_channel_idx < channel_count)
                             ? channels[active_channel_idx].name
                             : "(no channel)";
        add_label(scr, 12, CHAT_Y0 + 4, TXT_BODY, COL_WHITE, nm);

        char sub[48];
        if (region_scope[0]) {
            snprintf(sub, sizeof(sub), "  Region: %s", region_scope);
        } else {
            snprintf(sub, sizeof(sub), "  Region: (set in Settings)");
        }
        uint32_t sub_col = region_scope[0] ? COL_GRAY : COL_AMBER;
        add_label(scr, 12, CHAT_Y0 + 4 + TXT_BODY + 2, TXT_SMALL, sub_col, sub);
    }

    int input_y = h - CHAT_INPUT_H - FOOTER_H;
    int list_y0 = CHAT_Y0 + hdr_h + 4;
    int list_h  = input_y - list_y0;
    if (xSemaphoreTake(ch_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        render_msg_list_lvgl(scr, w, list_y0, list_h, ch_msgs, ch_head, ch_count, &ch_scroll, true);
        xSemaphoreGive(ch_mutex);
    }

    int iy = input_y;
    add_rect(scr, 0, iy, w, CHAT_INPUT_H, COL_PANEL);
    add_rect(scr, 0, iy, w, 2, chat_typing ? COL_ACCENT : COL_GREEN);
    if (chat_typing) {
        int ty = iy + (CHAT_INPUT_H - TXT_BODY) / 2;
        int pw = emoji_text(scr, 10, ty, TXT_BODY, COL_WHITE, "> ");
        int bw = emoji_text(scr, 10 + pw, ty, TXT_BODY, COL_WHITE, chat_input);
        add_label(scr, 10 + pw + bw, ty, TXT_BODY, COL_WHITE, "_");

        char ctr[12];
        snprintf(ctr, sizeof(ctr), "%d/%d", chat_input_len, MAX_INPUT_LEN);
        add_label(scr, w - text_w(ctr, TXT_SMALL) - 10, iy + (CHAT_INPUT_H - TXT_SMALL) / 2, TXT_SMALL, COL_GRAY, ctr);
    } else {
        add_label(scr, 10, iy + (CHAT_INPUT_H - TXT_SMALL) / 2, TXT_SMALL, COL_GREEN, "T: send channel message");
    }

    int fy = h - FOOTER_H;
    add_rect(scr, 0, fy, w, FOOTER_H, COL_HEADER);
    add_rect(scr, 0, fy, w, 1, COL_PANEL);
    int  hint_ty = fy + (FOOTER_H - TXT_SMALL) / 2;
    // Channel-slot usage (active channels incl. Public / capacity). Bottom-right.
    char chcc[12];
    snprintf(chcc, sizeof(chcc), "%d/%d", channel_count, CHANNELS_MAX);
    add_label(scr, w - text_w(chcc, TXT_SMALL) - 10, hint_ty, TXT_SMALL, COL_GRAY, chcc);
    if (chat_typing) {
        const char* hint = "Enter: send   Backspace: delete   ";
        add_label(scr, 10, hint_ty, TXT_SMALL, COL_HINT, hint);
        int hx = 10 + text_w(hint, TXT_SMALL);
        add_back_hint(scr, hx, hint_ty, ": cancel   ", TXT_SMALL);
        int xg     = TXT_SMALL / 2 - 1;
        int icon_x = hx + 2 * xg + 4 + text_w(": cancel   ", TXT_SMALL);
        int icon_y = fy + FOOTER_H / 2;
        add_circle(scr, icon_x + 6, icon_y, 6, -1, COL_GREEN, 2);
        add_label(scr, icon_x + 18, hint_ty, TXT_SMALL, COL_HINT, ": emoji");
        int icon2_x = icon_x + 18 + text_w(": emoji   ", TXT_SMALL);
        add_cloud(scr, icon2_x + 6, icon_y, 6, COL_BLUE);
        add_label(scr, icon2_x + 18, hint_ty, TXT_SMALL, COL_HINT, ": chars");
    } else {
        const char* hint = "Enter: type   W/S: scroll   R: clear   Tab: next   ";
        add_label(scr, 10, hint_ty, TXT_SMALL, COL_HINT, hint);
        add_back_hint(scr, 10 + text_w(hint, TXT_SMALL), hint_ty, ": list", TXT_SMALL);
    }

    if (emoji_picker_active && chat_typing) {
        render_emoji_picker_overlay_lvgl(scr, w, h);
    }
}
