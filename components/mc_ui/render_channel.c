// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "channels.h"
#include "chat.h"
#include "emoji.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"
#include "render.h"
#include "render_internal.h"
#include "settings_nvs.h"
#include "ui_state.h"

static void render_channel_list(int w, int h) {
    const int row_h    = 38;
    const int footer_h = FOOTER_H;

    pax_simple_rect(&fb, COL_PANEL, 0, CHAT_Y0, w, 28);
    pax_draw_text(&fb, COL_WHITE, FONT, TXT_BODY, 10, CHAT_Y0 + 4, "Channels");

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
            pax_simple_rect(&fb, COL_PANEL, 0, y, w, row_h - 1);
            pax_simple_rect(&fb, COL_ACCENT, 0, y, 5, row_h - 1);
        }
        pax_col_t name_col = is_sel ? COL_WHITE : COL_GRAY;
        int       text_y   = y + (row_h - TXT_BODY) / 2;

        if (is_active) {
            pax_draw_text(&fb, COL_GREEN, FONT, TXT_BODY, 18, text_y, ">");
        }
        pax_draw_text(&fb, name_col, FONT, TXT_BODY, 40, text_y, channels[i].name);

        if (channel_unread[i] > 0) {
            char ub[8];
            snprintf(ub, sizeof(ub), "%d", channel_unread[i] > 99 ? 99 : channel_unread[i]);
            pax_vec2f nsz = pax_text_size(FONT, TXT_BODY, channels[i].name);
            pax_vec2f usz = pax_text_size(FONT, TXT_SMALL, ub);
            int       bw  = (int)usz.x + 12;
            int       bx  = 40 + (int)nsz.x + 8;
            int       by  = y + (row_h - (TXT_SMALL + 4)) / 2;
            pax_simple_rect(&fb, COL_RED, bx, by, bw, TXT_SMALL + 4);
            pax_draw_text(&fb, COL_HEADER, FONT, TXT_SMALL, bx + 6, by + 2, ub);
        }

        char meta[24];
        snprintf(meta, sizeof(meta), "0x%02X", channels[i].hash);
        pax_vec2f msz = pax_text_size(FONT, TXT_TINY, meta);
        pax_draw_text(&fb, name_col, FONT, TXT_TINY, w - (int)msz.x - 14, y + (row_h - TXT_TINY) / 2, meta);
    }

    if (channel_adding) {
        int iy = h - CHAT_INPUT_H - footer_h;
        pax_simple_rect(&fb, COL_PANEL, 0, iy, w, CHAT_INPUT_H);
        pax_simple_rect(&fb, COL_ACCENT, 0, iy, w, 2);
        char disp[40];
        snprintf(disp, sizeof(disp), "add: %s_", field_edit_buf);
        pax_draw_text(&fb, COL_WHITE, FONT, TXT_BODY, 10, iy + (CHAT_INPUT_H - TXT_BODY) / 2, disp);
    }

    int fy = h - footer_h;
    pax_simple_rect(&fb, COL_HEADER, 0, fy, w, footer_h);
    pax_simple_rect(&fb, COL_PANEL, 0, fy, w, 1);
    const char* hint    = channel_adding
                              ? "Type name (e.g. #nl)   Enter: save   "
                              : (channel_list_cursor == 0 ? "W/S: nav   Enter: open   A: add   Tab: next   "
                                                          : "W/S: nav   Enter: open   A: add   D: delete   Tab: next   ");
    int         hint_ty = fy + (footer_h - TXT_SMALL) / 2;
    pax_draw_text(&fb, COL_HINT, FONT, TXT_SMALL, 10, hint_ty, hint);
    render_back_hint(10 + (int)pax_text_size(FONT, TXT_SMALL, hint).x, hint_ty, channel_adding ? ": cancel" : ": home",
                     TXT_SMALL);
}

void render_channel(void) {
    int w = (int)pax_buf_get_width(&fb);
    int h = (int)pax_buf_get_height(&fb);

    pax_background(&fb, COL_BG);
    render_tab_bar();

    if (channel_list_mode) {
        render_channel_list(w, h);
        return;
    }

    const int hdr_h = 50;
    pax_simple_rect(&fb, COL_PANEL, 0, CHAT_Y0, w, hdr_h);
    {
        const char* nm = (active_channel_idx >= 0 && active_channel_idx < channel_count)
                             ? channels[active_channel_idx].name
                             : "(no channel)";
        pax_draw_text(&fb, COL_WHITE, FONT, TXT_BODY, 12, CHAT_Y0 + 4, nm);

        char sub[48];
        if (region_scope[0]) {
            snprintf(sub, sizeof(sub), "  Region: %s", region_scope);
        } else {
            snprintf(sub, sizeof(sub), "  Region: (set in Settings)");
        }
        pax_col_t sub_col = region_scope[0] ? COL_GRAY : COL_AMBER;
        pax_draw_text(&fb, sub_col, FONT, TXT_SMALL, 12, CHAT_Y0 + 4 + TXT_BODY + 2, sub);
    }

    int input_y = h - CHAT_INPUT_H - FOOTER_H;
    int list_y0 = CHAT_Y0 + hdr_h + 4;
    int list_h  = input_y - list_y0;
    if (xSemaphoreTake(ch_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        render_msg_list(w, list_y0, list_h, ch_msgs, ch_head, ch_count, &ch_scroll, true);
        xSemaphoreGive(ch_mutex);
    }

    int iy = input_y;
    pax_simple_rect(&fb, COL_PANEL, 0, iy, w, CHAT_INPUT_H);
    pax_simple_rect(&fb, chat_typing ? COL_ACCENT : COL_GREEN, 0, iy, w, 2);
    if (chat_typing) {
        int ty = iy + (CHAT_INPUT_H - TXT_BODY) / 2;
        int pw = emoji_draw_text(&fb, COL_WHITE, FONT, TXT_BODY, 10, ty, "> ");
        int bw = emoji_draw_text(&fb, COL_WHITE, FONT, TXT_BODY, 10 + pw, ty, chat_input);
        pax_draw_text(&fb, COL_WHITE, FONT, TXT_BODY, 10 + pw + bw, ty, "_");

        char ctr[12];
        snprintf(ctr, sizeof(ctr), "%d/%d", chat_input_len, MAX_INPUT_LEN);
        pax_vec2f csz = pax_text_size(FONT, TXT_SMALL, ctr);
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, w - (int)csz.x - 10, iy + (CHAT_INPUT_H - TXT_SMALL) / 2, ctr);
    } else {
        pax_draw_text(&fb, COL_GREEN, FONT, TXT_SMALL, 10, iy + (CHAT_INPUT_H - TXT_SMALL) / 2,
                      "T: send channel message");
    }

    int fy = h - FOOTER_H;
    pax_simple_rect(&fb, COL_HEADER, 0, fy, w, FOOTER_H);
    pax_simple_rect(&fb, COL_PANEL, 0, fy, w, 1);
    int hint_ty = fy + (FOOTER_H - TXT_SMALL) / 2;
    if (chat_typing) {
        const char* hint = "Enter: send   Backspace: delete   ";
        pax_draw_text(&fb, COL_HINT, FONT, TXT_SMALL, 10, hint_ty, hint);
        int hx     = 10 + (int)pax_text_size(FONT, TXT_SMALL, hint).x;
        hx         = render_back_hint(hx, hint_ty, ": cancel   ", TXT_SMALL);
        int icon_x = hx;
        int icon_y = fy + FOOTER_H / 2;
        pax_outline_circle(&fb, COL_GREEN, icon_x + 6, icon_y, 6);
        pax_draw_text(&fb, COL_HINT, FONT, TXT_SMALL, icon_x + 18, hint_ty, ": emoji");
    } else {
        const char* hint = "T: type   W/S: scroll   R: clear   Tab: next   ";
        pax_draw_text(&fb, COL_HINT, FONT, TXT_SMALL, 10, hint_ty, hint);
        render_back_hint(10 + (int)pax_text_size(FONT, TXT_SMALL, hint).x, hint_ty, ": list", TXT_SMALL);
    }
}
