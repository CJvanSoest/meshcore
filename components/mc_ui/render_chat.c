// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "app_config.h"
#include "chat.h"
#include "contacts.h"
#include "emoji.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nodes.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"
#include "render.h"
#include "render_internal.h"

#define MSG_MAX_LINES 8

// Greedy word-wrap of `text` to fit `max_w` px at FONT/TXT_BODY. A word longer
// than a line is left on its own line (clipped). Measures with emoji_measure_text
// so emoji widths count. Explicit '\n' / '\r' in the source text are honoured
// as hard line breaks — otherwise pax_draw_text would render the newline glyph
// itself as a second line, but msg_wrap would still count this row as a single
// line, and the bubble below the next message would overlap the runaway text.
static int msg_wrap(const char* text, int max_w, char out[][MAX_MSG_TEXT], int max_lines) {
    int         nl                 = 0;
    char        line[MAX_MSG_TEXT] = {0};
    int         ll                 = 0;
    const char* p                  = text;
    while (*p && nl < max_lines) {
        if (*p == '\n' || *p == '\r') {
            // Flush the current line (possibly empty) and consume the CR/LF run.
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

        if (ll == 0 || emoji_measure_text(FONT, TXT_BODY, cand) <= max_w) {
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

void render_msg_list(int w, int list_y0, int list_h, chat_msg_t* msgs, int head, int count, int* scroll_p) {
    if (count == 0) {
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_BODY, 14, list_y0 + 10, "No messages yet. Press T to type.");
        return;
    }
    int sc = *scroll_p;
    if (sc > count) sc = count;
    if (sc < 1) sc = 1;
    *scroll_p = sc;

    const int line_h  = TXT_BODY + 4;
    const int meta_h  = TXT_TINY + 4;
    const int pad_x   = 8;   // inner horizontal padding within a bubble
    const int pad_y   = 5;   // inner vertical padding within a bubble
    const int gap     = 10;  // vertical gap between adjacent bubbles
    const int margin  = 14;  // outer horizontal margin from screen edges
    const int avail_w = w - 2 * margin - 2 * pad_x;
    char      lines[MSG_MAX_LINES][MAX_MSG_TEXT];

    pax_clip(&fb, 0, list_y0, w, list_h);
    int y = list_y0 + list_h;
    for (int li = sc - 1; li >= 0 && y > list_y0; li--) {
        int         ring = (head - count + li + MAX_CHAT_MSGS * 2) % MAX_CHAT_MSGS;
        chat_msg_t* m    = &msgs[ring];
        if (!m->active) continue;

        int nl = msg_wrap(m->text, avail_w, lines, MSG_MAX_LINES);

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
            if (m->is_mine && m->ack_state == 1)
                ack = "...";
            else if (m->is_mine && m->ack_state == 2)
                ack = "ack";
            int o = 0;
            if (tbuf[0]) o += snprintf(meta + o, sizeof(meta) - o, "%s", tbuf);
            if (hbuf[0]) o += snprintf(meta + o, sizeof(meta) - o, "%s%s", o ? " - " : "", hbuf);
            if (ack) snprintf(meta + o, sizeof(meta) - o, "%s%s", o ? " - " : "", ack);
        }

        // Each bubble: pad_y top + nl text lines + meta + pad_y bottom.
        // Step height adds an explicit gap between adjacent bubbles.
        int bubble_h = pad_y + nl * line_h + meta_h + pad_y;
        int mh       = bubble_h + gap;

        y -= mh;
        if (y + mh <= list_y0) break;

        int bubble_y = y + gap / 2;

        // Measure widest content (text lines + meta) so the bubble hugs the text.
        int maxw = 0;
        for (int k = 0; k < nl; k++) {
            int lw = emoji_measure_text(FONT, TXT_BODY, lines[k]);
            if (lw > maxw) maxw = lw;
        }
        if (meta[0] || m->is_mine) {
            const char* ml = meta[0] ? meta : "You";
            int         mw = (int)pax_text_size(FONT, TXT_TINY, ml).x;
            if (mw > maxw) maxw = mw;
        }
        int max_bubble_w = w - 2 * margin;
        int bubble_w     = maxw + 2 * pad_x;
        if (bubble_w > max_bubble_w) bubble_w = max_bubble_w;

        if (m->is_mine) {
            int bx = w - margin - bubble_w;
            pax_simple_rect(&fb, COL_PANEL, bx, bubble_y, bubble_w, bubble_h);
            for (int k = 0; k < nl; k++) {
                int lw = emoji_measure_text(FONT, TXT_BODY, lines[k]);
                emoji_draw_text(&fb, COL_BLUE, FONT, TXT_BODY, bx + bubble_w - pad_x - lw,
                                bubble_y + pad_y + k * line_h, lines[k]);
            }
            pax_col_t   mc  = (m->ack_state == 2) ? COL_GREEN : COL_GRAY;
            const char* ml  = meta[0] ? meta : "You";
            pax_vec2f   msz = pax_text_size(FONT, TXT_TINY, ml);
            pax_draw_text(&fb, mc, FONT, TXT_TINY, bx + bubble_w - pad_x - (int)msz.x, bubble_y + pad_y + nl * line_h,
                          ml);
        } else {
            int bx = margin;
            pax_simple_rect(&fb, COL_HEADER, bx, bubble_y, bubble_w, bubble_h);
            pax_simple_rect(&fb, COL_ACCENT, bx, bubble_y, 3, bubble_h);
            for (int k = 0; k < nl; k++) {
                emoji_draw_text(&fb, COL_WHITE, FONT, TXT_BODY, bx + pad_x, bubble_y + pad_y + k * line_h, lines[k]);
            }
            if (meta[0]) {
                pax_draw_text(&fb, COL_GRAY, FONT, TXT_TINY, bx + pad_x, bubble_y + pad_y + nl * line_h, meta);
            }
        }
    }
    pax_noclip(&fb);
}

void render_chat(void) {
    int w = (int)pax_buf_get_width(&fb);
    int h = (int)pax_buf_get_height(&fb);

    pax_background(&fb, COL_BG);
    render_tab_bar();

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
            pax_draw_text(&fb, COL_AMBER, FONT, TXT_BODY, 16, inbox_y0 + 18, "No conversations yet");
            pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 16, inbox_y0 + 18 + TXT_BODY + 6,
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
                    pax_simple_rect(&fb, COL_PANEL, 0, y, w, row_h - 2);
                    pax_simple_rect(&fb, COL_ACCENT, 0, y, 5, row_h - 2);
                } else if (row_unread > 0) {
                    // Subtle stripe on left edge so unread rows pop without
                    // stealing the cursor's amber/accent treatment.
                    pax_simple_rect(&fb, COL_RED, 0, y, 3, row_h - 2);
                }
                pax_simple_rect(&fb, COL_PANEL, 12, y + row_h - 1, w - 24, 1);

                int       av_x = 18, av_y = y + (row_h - 36) / 2, av_d = 36;
                pax_col_t av_bg = is_active ? COL_AMBER : COL_BLUE;
                pax_simple_rect(&fb, av_bg, av_x, av_y, av_d, av_d);
                char      init[2] = {(char)(name[0] ? toupper((unsigned char)name[0]) : '?'), 0};
                pax_vec2f isz     = pax_text_size(FONT, TXT_TITLE, init);
                pax_draw_text(&fb, COL_HEADER, FONT, TXT_TITLE, av_x + (av_d - (int)isz.x) / 2,
                              av_y + (av_d - TXT_TITLE) / 2 - 1, init);

                pax_col_t name_col = is_cursor ? COL_WHITE : COL_WHITE;
                int       name_x   = av_x + av_d + 12;
                pax_draw_text(&fb, name_col, FONT, TXT_BODY, name_x, y + 6, name);

                if (row_unread > 0) {
                    char ub[8];
                    snprintf(ub, sizeof(ub), "%d", row_unread > 99 ? 99 : row_unread);
                    pax_vec2f nsz = pax_text_size(FONT, TXT_BODY, name);
                    pax_vec2f usz = pax_text_size(FONT, TXT_SMALL, ub);
                    int       bw  = (int)usz.x + 12;
                    int       bx  = name_x + (int)nsz.x + 8;
                    int       by  = y + 6;
                    pax_simple_rect(&fb, COL_RED, bx, by, bw, TXT_SMALL + 4);
                    pax_draw_text(&fb, COL_HEADER, FONT, TXT_SMALL, bx + 6, by + 2, ub);
                }

                const char* rl = role_label(role);
                char        sub[64];
                if (row_unread > 0) {
                    snprintf(sub, sizeof(sub), "%s  ·  %d new", rl, row_unread > 99 ? 99 : row_unread);
                } else if (is_active) {
                    snprintf(sub, sizeof(sub), "%s  ·  active DM", rl);
                } else {
                    snprintf(sub, sizeof(sub), "%s  ·  saved contact", rl);
                }
                pax_col_t sub_col = (row_unread > 0) ? COL_RED : COL_GRAY;
                pax_draw_text(&fb, sub_col, FONT, TXT_SMALL, av_x + av_d + 12, y + 6 + TXT_BODY + 4, sub);

                if (is_cursor) {
                    const char* cta = "Enter ›";
                    pax_vec2f   sz  = pax_text_size(FONT, TXT_SMALL, cta);
                    pax_draw_text(&fb, COL_AMBER, FONT, TXT_SMALL, w - (int)sz.x - 12, y + (row_h - TXT_SMALL) / 2,
                                  cta);
                }
            }

            if (idx_count > rows_vis) {
                char sc[24];
                snprintf(sc, sizeof(sc), "%d/%d", dm_inbox_cursor + 1, idx_count);
                pax_vec2f sz = pax_text_size(FONT, TXT_SMALL, sc);
                pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, w - (int)sz.x - 10, h - footer_h - TXT_SMALL - 2, sc);
            }
        }

        int fy_base = h - footer_h;
        pax_simple_rect(&fb, COL_HEADER, 0, fy_base, w, footer_h);
        pax_simple_rect(&fb, COL_PANEL, 0, fy_base, w, 1);
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 10, fy_base + (footer_h - TXT_SMALL) / 2,
                      "W/S: nav   Enter: open   D: delete   Tab: next");
        return;
    }

    int input_y = h - CHAT_INPUT_H - FOOTER_H;
    int list_y0 = CHAT_Y0 + 32;
    int list_h  = input_y - list_y0;
    pax_simple_rect(&fb, COL_PANEL, 0, CHAT_Y0, w, 28);
    {
        char hdr[MESHCORE_MAX_NAME_SIZE + 24];
        snprintf(hdr, sizeof(hdr), "‹  %s", dm_target_set ? dm_target_name : "(no target)");
        pax_draw_text(&fb, COL_WHITE, FONT, TXT_BODY, 10, CHAT_Y0 + 4, hdr);
    }

    if (xSemaphoreTake(chat_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        render_msg_list(w, list_y0, list_h, chat_msgs, chat_head, chat_count, &chat_scroll);
        xSemaphoreGive(chat_mutex);
    }

    int iy = input_y;
    pax_simple_rect(&fb, COL_PANEL, 0, iy, w, CHAT_INPUT_H);
    pax_simple_rect(&fb, chat_typing ? COL_ACCENT : COL_AMBER, 0, iy, w, 2);
    if (chat_typing) {
        char prefix[MESHCORE_MAX_NAME_SIZE + 8];
        snprintf(prefix, sizeof(prefix), "DM %s> ", dm_target_name);
        int ty = iy + (CHAT_INPUT_H - TXT_BODY) / 2;
        int pw = emoji_draw_text(&fb, COL_WHITE, FONT, TXT_BODY, 10, ty, prefix);
        int bw = emoji_draw_text(&fb, COL_WHITE, FONT, TXT_BODY, 10 + pw, ty, chat_input);
        pax_draw_text(&fb, COL_WHITE, FONT, TXT_BODY, 10 + pw + bw, ty, "_");

        char ctr[12];
        snprintf(ctr, sizeof(ctr), "%d/%d", chat_input_len, MAX_INPUT_LEN);
        pax_vec2f csz = pax_text_size(FONT, TXT_SMALL, ctr);
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, w - (int)csz.x - 10, iy + (CHAT_INPUT_H - TXT_SMALL) / 2, ctr);
    } else {
        pax_draw_text(&fb, COL_AMBER, FONT, TXT_SMALL, 10, iy + (CHAT_INPUT_H - TXT_SMALL) / 2, "T: type message");
    }

    int fy = h - FOOTER_H;
    pax_simple_rect(&fb, COL_HEADER, 0, fy, w, FOOTER_H);
    pax_simple_rect(&fb, COL_PANEL, 0, fy, w, 1);
    if (chat_typing) {
        const char* hint = "Enter: send   ESC: cancel   Backspace: delete   ";
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 10, fy + (FOOTER_H - TXT_SMALL) / 2, hint);
        pax_vec2f hsz    = pax_text_size(FONT, TXT_SMALL, hint);
        int       icon_x = 10 + (int)hsz.x;
        int       icon_y = fy + FOOTER_H / 2;
        pax_outline_circle(&fb, COL_GREEN, icon_x + 6, icon_y, 6);
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, icon_x + 18, fy + (FOOTER_H - TXT_SMALL) / 2, ": emoji");
    } else {
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 10, fy + (FOOTER_H - TXT_SMALL) / 2,
                      "T: type   W/S: scroll   ESC: back to inbox   Tab: next tab");
    }
}
