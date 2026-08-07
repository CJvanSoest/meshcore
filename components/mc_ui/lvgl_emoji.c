// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// inline emoji and the emoji picker overlay.

#include <stdio.h>
#include <string.h>
#include "chat.h"
#include "emoji_table.h"
#include "history.h"
#include "lvgl.h"
#include "lvgl_internal.h"
#include "render.h"
#include "special_table.h"

// ── Inline emoji (chat) ──────────────────────────────────────────────────────
// The 32x32 Twemoji bitmaps in emoji_bitmaps.c are ARGB8888 (0xAARRGGBB); on
// this little-endian target that byte order is exactly LVGL's
// LV_COLOR_FORMAT_ARGB8888 (B,G,R,A), so each flash array wraps straight into an
// lv_image_dsc_t with no copy.

extern const uint32_t* const EMOJI_BITMAPS[];
#define EMOJI_PX 32

static lv_image_dsc_t s_emoji_dsc[EMOJI_COUNT];
static bool           s_emoji_dsc_ready;

static void emoji_dsc_init(void) {
    if (s_emoji_dsc_ready) {
        return;
    }
    for (int i = 0; i < EMOJI_COUNT; i++) {
        s_emoji_dsc[i].header.magic  = LV_IMAGE_HEADER_MAGIC;
        s_emoji_dsc[i].header.cf     = LV_COLOR_FORMAT_ARGB8888;
        s_emoji_dsc[i].header.flags  = 0;
        s_emoji_dsc[i].header.w      = EMOJI_PX;
        s_emoji_dsc[i].header.h      = EMOJI_PX;
        s_emoji_dsc[i].header.stride = EMOJI_PX * 4;
        s_emoji_dsc[i].data          = (const uint8_t*)EMOJI_BITMAPS[i];
        s_emoji_dsc[i].data_size     = EMOJI_PX * EMOJI_PX * 4;
    }
    s_emoji_dsc_ready = true;
}

// Inline emoji diameter for a given text size — matches emoji.c exactly.
static int emoji_inline_d(int size) {
    int d = (int)(size * 1.1f);
    if (d < 12) d = 12;
    return d;
}

// Draw (parent != NULL) or just measure (parent == NULL) `text` at (x, y) with
// inline emoji, returning the advance width. Faithful port of emoji.c's
// render_or_measure so the wrap/measure math lines up 1:1 with what's drawn.
int emoji_text(lv_obj_t* parent, int x, int y, int size, uint32_t col, const char* text) {
    emoji_dsc_init();
    int  dx = 0;
    int  d  = emoji_inline_d(size);
    int  r  = d / 2;
    char run[256];
    int  run_len = 0;

    int i = 0;
    while (text[i]) {
        uint32_t cp  = 0;
        int      adv = utf8_decode(&text[i], &cp);
        if (adv <= 0) {
            if (run_len < (int)sizeof(run) - 1) run[run_len++] = '?';
            i++;
            continue;
        }
        int idx = (cp >= 0x80) ? emoji_lookup_by_codepoint(cp) : -1;
        if (idx >= 0) {
            if (run_len > 0) {
                run[run_len] = '\0';
                if (parent) add_label(parent, x + dx, y, size, col, run);
                dx      += text_w(run, size);
                run_len  = 0;
            }
            if (parent) {
                lv_obj_t* im = lv_image_create(parent);
                lv_image_set_src(im, &s_emoji_dsc[idx]);
                lv_image_set_antialias(im, true);
                lv_image_set_pivot(im, 0, 0);
                lv_image_set_scale(im, (256 * d) / EMOJI_PX);
                lv_obj_set_pos(im, x + dx, y + size / 2 - r);
            }
            dx += d + 1;
            i  += adv;
            continue;
        }
        if (cp >= 0x80) {
            // Non-emoji, non-ASCII: pass the raw UTF-8 bytes into the text run so
            // the extended font draws the glyph (umlauts, accents, °, €, …). Only
            // genuinely undrawable codepoints collapse to '?'.
            if (special_font_covers(cp) && run_len + adv < (int)sizeof(run) - 1) {
                memcpy(&run[run_len], &text[i], (size_t)adv);
                run_len += adv;
            } else if (run_len < (int)sizeof(run) - 1) {
                run[run_len++] = '?';
            }
            i += adv;
            continue;
        }
        if (run_len < (int)sizeof(run) - 1) run[run_len++] = (char)cp;
        i += adv;
    }
    if (run_len > 0) {
        run[run_len] = '\0';
        if (parent) add_label(parent, x + dx, y, size, col, run);
        dx += text_w(run, size);
    }
    return dx;
}

// Draw emoji `idx` centred at (cx, cy) with diameter `d` — the LVGL mirror of
// emoji.c's emoji_draw (top-left at cx-d/2, cy-d/2, scaled from EMOJI_PX).
static void emoji_image(lv_obj_t* parent, int idx, int cx, int cy, int d) {
    if (idx < 0 || idx >= EMOJI_COUNT) {
        return;
    }
    emoji_dsc_init();
    lv_obj_t* im = lv_image_create(parent);
    lv_image_set_src(im, &s_emoji_dsc[idx]);
    lv_image_set_antialias(im, true);
    lv_image_set_pivot(im, 0, 0);
    lv_image_set_scale(im, (256 * d) / EMOJI_PX);
    lv_obj_set_pos(im, cx - d / 2, cy - d / 2);
}

// Draw a special-character bank entry as a centred font glyph (the F5 bank is
// plain UTF-8, rendered from the extended Montserrat display font, unlike the
// emoji bank which is bitmap art).
static void special_glyph(lv_obj_t* parent, int idx, int cx, int cy) {
    const emoji_entry_t* e = picker_entry(idx);  // active bank == PICKER_SPECIAL here
    if (!e) {
        return;
    }
    int gw = text_w(e->utf8, TXT_TITLE);
    add_label(parent, cx - gw / 2, cy - TXT_TITLE / 2 - 2, TXT_TITLE, COL_WHITE, e->utf8);
}

// Paged 4x5 character-picker overlay drawn on top of the Chat/Channel base view
// while typing. Serves both banks: F4 emoji (bitmaps) and F5 special characters
// (font glyphs). Pixel-matched port of render_emoji_picker_overlay() in render.c.
void render_emoji_picker_overlay_lvgl(lv_obj_t* scr, int w, int h) {
    const bool special  = (emoji_picker_mode == PICKER_SPECIAL);
    const int  count    = picker_count();
    const int  cols     = 4;
    const int  vis_rows = 5;
    const int  per_page = cols * vis_rows;
    const int  cell     = 48;
    const int  pad      = 14;
    const int  grid_w   = cols * cell;  // width of the glyph grid itself
    const int  panel_h  = vis_rows * cell + 2 * pad + TXT_SMALL + 6;

    if (emoji_picker_cursor < 0) emoji_picker_cursor = 0;
    if (emoji_picker_cursor >= count) emoji_picker_cursor = count - 1;
    int pages = (count + per_page - 1) / per_page;
    int page  = emoji_picker_cursor / per_page;
    int start = page * per_page;

    // Header: title (left) + scroll hint (right). Size the panel so the two never
    // overlap — widen past the grid when the header text needs the room.
    const char* what = special ? "Pick character" : "Pick emoji";
    char        title[40];
    if (pages > 1) {
        snprintf(title, sizeof(title), "%s  %d/%d", what, page + 1, pages);
    } else {
        snprintf(title, sizeof(title), "%s", what);
    }
    const char* nav       = "W/S: scroll";
    int         header_w  = text_w(title, TXT_SMALL) + (pages > 1 ? 16 + text_w(nav, TXT_SMALL) : 0);
    int         content_w = (grid_w > header_w) ? grid_w : header_w;
    int         panel_w   = content_w + 2 * pad;
    int         panel_x   = (w - panel_w) / 2;
    int         panel_y   = h - CHAT_INPUT_H - FOOTER_H - panel_h - 4;
    if (panel_y < TAB_BAR_H + 4) panel_y = TAB_BAR_H + 4;

    add_rect(scr, panel_x, panel_y, panel_w, panel_h, COL_HEADER);
    add_rect(scr, panel_x, panel_y, panel_w, 2, COL_ACCENT);

    add_label(scr, panel_x + pad, panel_y + 4, TXT_SMALL, COL_AMBER, title);
    if (pages > 1) {
        add_label(scr, panel_x + panel_w - text_w(nav, TXT_SMALL) - pad, panel_y + 4, TXT_SMALL, COL_GRAY, nav);
    }

    int grid_x = panel_x + (panel_w - cols * cell) / 2;  // centre the grid in the panel
    int grid_y = panel_y + 6 + TXT_SMALL;

    for (int i = start; i < start + per_page && i < count; i++) {
        int  local = i - start;
        int  r     = local / cols;
        int  c     = local % cols;
        int  cx    = grid_x + c * cell + cell / 2;
        int  cy    = grid_y + r * cell + cell / 2;
        bool sel   = (i == emoji_picker_cursor);
        if (sel) {
            add_rect(scr, cx - cell / 2 + 2, cy - cell / 2 + 2, cell - 4, cell - 4, COL_PANEL);
        }
        if (special) {
            special_glyph(scr, i, cx, cy);
        } else {
            emoji_image(scr, i, cx, cy, 2 * (cell / 2 - 6));
        }
    }
}
