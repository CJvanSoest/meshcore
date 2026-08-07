// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// VIEW_CHAT and the shared message ring renderer.

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

// ── Shared chat-message ring renderer (DM + Channel) ─────────────────────────
// Pixel-matched port of render_msg_list() / msg_wrap() in render_chat.c.

#define MSG_MAX_LINES 8

// Greedy word-wrap to fit max_w px at TXT_BODY, measuring with the same inline-
// emoji metric used to draw, so a bubble always fits its own content.
static int msg_wrap_lvgl(const char* text, int max_w, char out[][MAX_MSG_TEXT], int max_lines) {
    int         nl                 = 0;
    char        line[MAX_MSG_TEXT] = {0};
    int         ll                 = 0;
    const char* p                  = text;
    while (*p && nl < max_lines) {
        if (*p == '\n' || *p == '\r') {
            memcpy(out[nl], line, ll + 1);
            nl++;
            ll      = 0;
            line[0] = 0;
            while (*p == '\n' || *p == '\r') p++;
            continue;
        }
        const char* w0 = p;
        while (*p && *p != ' ' && *p != '\n' && *p != '\r') p++;
        while (*p == ' ') p++;
        int wlen = (int)(p - w0);
        if (wlen > MAX_MSG_TEXT - 1) wlen = MAX_MSG_TEXT - 1;

        char cand[MAX_MSG_TEXT];
        int  copy = wlen;
        if (ll + copy > MAX_MSG_TEXT - 1) copy = MAX_MSG_TEXT - 1 - ll;
        if (ll) memcpy(cand, line, ll);
        memcpy(cand + ll, w0, copy);
        cand[ll + copy] = 0;

        if (ll == 0 || emoji_text(NULL, 0, 0, TXT_BODY, 0, cand) <= max_w) {
            memcpy(line, cand, ll + copy + 1);
            ll += copy;
        } else {
            memcpy(out[nl], line, ll + 1);
            nl++;
            if (nl >= max_lines) break;
            ll = (wlen < MAX_MSG_TEXT) ? wlen : MAX_MSG_TEXT - 1;
            memcpy(line, w0, ll);
            line[ll] = 0;
        }
    }
    if (ll > 0 && nl < max_lines) {
        memcpy(out[nl], line, ll + 1);
        nl++;
    }
    if (nl == 0) {
        out[0][0] = 0;
        nl        = 1;
    }
    return nl;
}

void render_msg_list_lvgl(lv_obj_t* scr, int w, int list_y0, int list_h, chat_msg_t* msgs, int head, int count,
                          int* scroll_p, bool is_channel) {
    if (count == 0) {
        add_label(scr, 14, list_y0 + 10, TXT_BODY, COL_GRAY, "No messages yet. Press Enter to type.");
        return;
    }
    int sc = *scroll_p;
    if (sc > count) sc = count;
    if (sc < 1) sc = 1;
    *scroll_p = sc;

    const int line_h  = TXT_BODY + 4;
    const int meta_h  = TXT_TINY + 4;
    const int pad_x   = 8;
    const int pad_y   = 5;
    const int gap     = 10;
    const int margin  = 14;
    const int avail_w = w - 2 * margin - 2 * pad_x;
    char      lines[MSG_MAX_LINES][MAX_MSG_TEXT];

    // Clip container at the list region: LVGL clips children to it, so a tall
    // top bubble can't bleed into the header (the PAX path used pax_clip). All
    // child coordinates below are local to this container.
    lv_obj_t* lst = lv_obj_create(scr);
    lv_obj_remove_style_all(lst);
    lv_obj_set_pos(lst, 0, list_y0);
    lv_obj_set_size(lst, w, list_h);
    lv_obj_clear_flag(lst, LV_OBJ_FLAG_SCROLLABLE);

    int y = list_h;
    for (int li = sc - 1; li >= 0 && y > 0; li--) {
        int         ring = (head - count + li + MAX_CHAT_MSGS * 2) % MAX_CHAT_MSGS;
        chat_msg_t* m    = &msgs[ring];
        if (!m->active) continue;

        int nl = msg_wrap_lvgl(m->text, avail_w, lines, MSG_MAX_LINES);

        char meta[64] = {0};
        {
            char tbuf[12] = {0};
            if (m->timestamp_unix > 0) {
                time_t    t = (time_t)m->timestamp_unix;
                struct tm lt;
                localtime_r(&t, &lt);
                snprintf(tbuf, sizeof(tbuf), "%02d:%02d", lt.tm_hour, lt.tm_min);
            }
            char hbuf[16] = {0};
            if (!m->is_mine && m->hops != 0xFF) snprintf(hbuf, sizeof(hbuf), "%uh", (unsigned)m->hops);
            const char* ack = NULL;
            if (m->is_mine) {
                if (m->ack_state == 1)
                    ack = is_channel ? "sent" : "...";
                else if (m->ack_state == 2)
                    ack = is_channel ? "relayed" : "ack";
                else if (m->ack_state == 3)
                    ack = "not sent";
            }
            int o = 0;
            if (tbuf[0]) o += snprintf(meta + o, sizeof(meta) - o, "%s", tbuf);
            if (hbuf[0]) o += snprintf(meta + o, sizeof(meta) - o, "%s%s", o ? " - " : "", hbuf);
            if (ack) snprintf(meta + o, sizeof(meta) - o, "%s%s", o ? " - " : "", ack);
        }

        int bubble_h = pad_y + nl * line_h + meta_h + pad_y;
        int mh       = bubble_h + gap;

        y -= mh;
        if (y + mh <= 0) break;

        int bubble_y = y + gap / 2;

        int maxw = 0;
        for (int k = 0; k < nl; k++) {
            int lw = emoji_text(NULL, 0, 0, TXT_BODY, 0, lines[k]);
            if (lw > maxw) maxw = lw;
        }
        if (meta[0] || m->is_mine) {
            const char* ml = meta[0] ? meta : "You";
            int         mw = text_w(ml, TXT_TINY);
            if (mw > maxw) maxw = mw;
        }
        int max_bubble_w = w - 2 * margin;
        int bubble_w     = maxw + 2 * pad_x;
        if (bubble_w > max_bubble_w) bubble_w = max_bubble_w;

        if (m->is_mine) {
            int bx = w - margin - bubble_w;
            add_rect(lst, bx, bubble_y, bubble_w, bubble_h, COL_PANEL);
            for (int k = 0; k < nl; k++) {
                int lw = emoji_text(NULL, 0, 0, TXT_BODY, 0, lines[k]);
                emoji_text(lst, bx + bubble_w - pad_x - lw, bubble_y + pad_y + k * line_h, TXT_BODY, COL_BLUE,
                           lines[k]);
            }
            uint32_t    mcol = (m->ack_state == 2) ? COL_GREEN : (m->ack_state == 3) ? COL_RED : COL_GRAY;
            const char* ml   = meta[0] ? meta : "You";
            int         mw   = text_w(ml, TXT_TINY);
            add_label(lst, bx + bubble_w - pad_x - mw, bubble_y + pad_y + nl * line_h, TXT_TINY, mcol, ml);
        } else {
            int bx = margin;
            add_rect(lst, bx, bubble_y, bubble_w, bubble_h, COL_HEADER);
            add_rect(lst, bx, bubble_y, 3, bubble_h, COL_ACCENT);
            for (int k = 0; k < nl; k++) {
                emoji_text(lst, bx + pad_x, bubble_y + pad_y + k * line_h, TXT_BODY, COL_WHITE, lines[k]);
            }
            if (meta[0]) {
                add_label(lst, bx + pad_x, bubble_y + pad_y + nl * line_h, TXT_TINY, COL_GRAY, meta);
            }
        }
    }
}

// ── VIEW_CHAT (DM inbox + conversation) ──────────────────────────────────────
// Pixel-matched port of render_chat.c. The emoji-picker overlay reachable while
// typing stays on the PAX path (lvgl_view_active reports false while it's up).

void render_chat_lvgl(void) {
    int       w   = (int)lvgl_port_width();
    int       h   = (int)lvgl_port_height();
    lv_obj_t* scr = begin_screen(COL_BG);
    pt_reset();
    tab_bar_lvgl(scr);

    if (dm_inbox_mode) {
        int inbox_y0 = TAB_BAR_H + 6;
        int footer_h = 36;
        int inbox_h  = h - inbox_y0 - footer_h;
        int row_h    = 56;
        int rows_vis = inbox_h / row_h;
        if (rows_vis < 1) rows_vis = 1;

        int  idx_map[MAX_CONTACTS + 1];
        int  idx_count     = 0;
        bool active_on_top = dm_target_set;
        if (active_on_top) idx_map[idx_count++] = -1;
        if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            for (int i = 0; i < contact_count && idx_count < MAX_CONTACTS + 1; i++) {
                if (active_on_top && memcmp(contacts[i].pub_key, dm_target_pub, MESHCORE_PUB_KEY_SIZE) == 0) continue;
                idx_map[idx_count++] = i;
            }
            xSemaphoreGive(node_mutex);
        }

        if (idx_count == 0) {
            add_label(scr, 16, inbox_y0 + 18, TXT_BODY, COL_AMBER, "No conversations yet");
            add_label(scr, 16, inbox_y0 + 18 + TXT_BODY + 6, TXT_SMALL, COL_GRAY,
                      "Open the Nodes tab and press Enter on a contact.");
        } else {
            if (dm_inbox_cursor >= idx_count) dm_inbox_cursor = idx_count - 1;
            if (dm_inbox_cursor < 0) dm_inbox_cursor = 0;
            if (dm_inbox_cursor < dm_inbox_scroll) dm_inbox_scroll = dm_inbox_cursor;
            if (dm_inbox_cursor >= dm_inbox_scroll + rows_vis) dm_inbox_scroll = dm_inbox_cursor - rows_vis + 1;
            int max_scroll = idx_count - rows_vis;
            if (max_scroll < 0) max_scroll = 0;
            if (dm_inbox_scroll > max_scroll) dm_inbox_scroll = max_scroll;
            if (dm_inbox_scroll < 0) dm_inbox_scroll = 0;

            for (int row = 0; row < rows_vis; row++) {
                int li = row + dm_inbox_scroll;
                if (li >= idx_count) break;
                int                    e         = idx_map[li];
                bool                   is_active = (e == -1);
                bool                   is_cursor = (li == dm_inbox_cursor);
                const char*            name;
                meshcore_device_role_t role;
                if (is_active) {
                    name   = dm_target_name;
                    int ci = contact_find(dm_target_pub);
                    role   = (ci >= 0) ? (meshcore_device_role_t)contacts[ci].role : MESHCORE_DEVICE_ROLE_CHAT_NODE;
                } else {
                    name = contacts[e].alias;
                    role = (meshcore_device_role_t)contacts[e].role;
                }

                int row_slot   = is_active ? contact_find(dm_target_pub) : e;
                int row_unread = (row_slot >= 0) ? contact_unread[row_slot] : 0;

                int y = inbox_y0 + row * row_h;
                if (is_cursor) {
                    add_rect(scr, 0, y, w, row_h - 2, COL_PANEL);
                    add_rect(scr, 0, y, 5, row_h - 2, COL_ACCENT);
                } else if (row_unread > 0) {
                    add_rect(scr, 0, y, 3, row_h - 2, COL_RED);
                }
                add_rect(scr, 12, y + row_h - 1, w - 24, 1, COL_PANEL);

                int      av_x = 18, av_y = y + (row_h - 36) / 2, av_d = 36;
                uint32_t av_bg = is_active ? COL_AMBER : COL_BLUE;
                add_rect(scr, av_x, av_y, av_d, av_d, av_bg);
                char init[2] = {(char)(name[0] ? toupper((unsigned char)name[0]) : '?'), 0};
                add_label(scr, av_x + (av_d - text_w(init, TXT_TITLE)) / 2, av_y + (av_d - TXT_TITLE) / 2 - 1,
                          TXT_TITLE, COL_HEADER, init);

                int name_x = av_x + av_d + 12;
                add_label(scr, name_x, y + 6, TXT_BODY, COL_WHITE, name);

                if (row_unread > 0) {
                    char ub[8];
                    snprintf(ub, sizeof(ub), "%d", row_unread > 99 ? 99 : row_unread);
                    int nw = text_w(name, TXT_BODY);
                    int bw = text_w(ub, TXT_SMALL) + 12;
                    int bx = name_x + nw + 8;
                    int by = y + 6;
                    add_rect(scr, bx, by, bw, TXT_SMALL + 4, COL_RED);
                    add_label(scr, bx + 6, by + 2, TXT_SMALL, COL_HEADER, ub);
                }

                const char* rl = role_label(role);
                char        sub[64];
                if (row_unread > 0) {
                    snprintf(sub, sizeof(sub), "%s  -  %d new", rl, row_unread > 99 ? 99 : row_unread);
                } else if (is_active) {
                    snprintf(sub, sizeof(sub), "%s  -  active DM", rl);
                } else {
                    snprintf(sub, sizeof(sub), "%s  -  saved contact", rl);
                }
                uint32_t sub_col = (row_unread > 0) ? COL_RED : COL_GRAY;
                add_label(scr, av_x + av_d + 12, y + 6 + TXT_BODY + 4, TXT_SMALL, sub_col, sub);

                if (is_cursor) {
                    const char* cta = "Enter >";
                    add_label(scr, w - text_w(cta, TXT_SMALL) - 12, y + (row_h - TXT_SMALL) / 2, TXT_SMALL, COL_AMBER,
                              cta);
                }
            }

            if (idx_count > rows_vis) {
                char sc2[24];
                snprintf(sc2, sizeof(sc2), "%d/%d", dm_inbox_cursor + 1, idx_count);
                add_label(scr, w - text_w(sc2, TXT_SMALL) - 10, h - footer_h - TXT_SMALL - 2, TXT_SMALL, COL_GRAY, sc2);
            }
        }

        int fy_base = h - footer_h;
        add_rect(scr, 0, fy_base, w, footer_h, COL_HEADER);
        add_rect(scr, 0, fy_base, w, 1, COL_PANEL);
        const char* inbox_hint = "W/S: nav   Enter: open   D: delete   Tab: next   ";
        int         ih_ty      = fy_base + (footer_h - TXT_SMALL) / 2;
        add_label(scr, 10, ih_ty, TXT_SMALL, COL_HINT, inbox_hint);
        add_back_hint(scr, 10 + text_w(inbox_hint, TXT_SMALL), ih_ty, ": home", TXT_SMALL);
        // Saved DM conversations / limit (= contacts). Bottom-right, shown as
        // soon as the DM tile opens.
        char dmcc[12];
        snprintf(dmcc, sizeof(dmcc), "%d/%d", contact_count, MAX_CONTACTS);
        add_label(scr, w - text_w(dmcc, TXT_SMALL) - 10, ih_ty, TXT_SMALL, COL_GRAY, dmcc);
        return;
    }

    int input_y = h - CHAT_INPUT_H - FOOTER_H;
    int list_y0 = CHAT_Y0 + 32;
    int list_h  = input_y - list_y0;
    add_rect(scr, 0, CHAT_Y0, w, 28, COL_PANEL);
    {
        char hdr[MESHCORE_MAX_NAME_SIZE + 24];
        snprintf(hdr, sizeof(hdr), "<  %s", dm_target_set ? dm_target_name : "(no target)");
        add_label(scr, 10, CHAT_Y0 + 4, TXT_BODY, COL_WHITE, hdr);
    }

    if (xSemaphoreTake(chat_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        render_msg_list_lvgl(scr, w, list_y0, list_h, chat_msgs, chat_head, chat_count, &chat_scroll, false);
        xSemaphoreGive(chat_mutex);
    }

    int iy = input_y;
    add_rect(scr, 0, iy, w, CHAT_INPUT_H, COL_PANEL);
    add_rect(scr, 0, iy, w, 2, chat_typing ? COL_ACCENT : COL_AMBER);
    if (chat_typing) {
        char prefix[MESHCORE_MAX_NAME_SIZE + 8];
        snprintf(prefix, sizeof(prefix), "DM %s> ", dm_target_name);
        int ty = iy + (CHAT_INPUT_H - TXT_BODY) / 2;
        int pw = emoji_text(scr, 10, ty, TXT_BODY, COL_WHITE, prefix);
        int bw = emoji_text(scr, 10 + pw, ty, TXT_BODY, COL_WHITE, chat_input);
        add_label(scr, 10 + pw + bw, ty, TXT_BODY, COL_WHITE, "_");

        char ctr[12];
        snprintf(ctr, sizeof(ctr), "%d/%d", chat_input_len, MAX_INPUT_LEN);
        add_label(scr, w - text_w(ctr, TXT_SMALL) - 10, iy + (CHAT_INPUT_H - TXT_SMALL) / 2, TXT_SMALL, COL_GRAY, ctr);
    } else {
        add_label(scr, 10, iy + (CHAT_INPUT_H - TXT_SMALL) / 2, TXT_SMALL, COL_AMBER, "Enter: type message");
    }

    int fy = h - FOOTER_H;
    add_rect(scr, 0, fy, w, FOOTER_H, COL_HEADER);
    add_rect(scr, 0, fy, w, 1, COL_PANEL);
    int hint_ty = fy + (FOOTER_H - TXT_SMALL) / 2;
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
        const char* hint = "Enter: type   W/S: scroll   Tab: next tab   ";
        add_label(scr, 10, hint_ty, TXT_SMALL, COL_HINT, hint);
        add_back_hint(scr, 10 + text_w(hint, TXT_SMALL), hint_ty, ": back to inbox", TXT_SMALL);
    }

    if (emoji_picker_active && chat_typing) {
        render_emoji_picker_overlay_lvgl(scr, w, h);
    }
}
