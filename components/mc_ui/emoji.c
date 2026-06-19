// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// Twemoji-based inline emoji rendering. Wire format is full UTF-8 (Unicode
// codepoints from the U+1F6xx Emoticons block) so messages round-trip 1:1
// with other MeshCore clients. Glyphs come from the embedded 32x32 Twemoji
// bitmaps (see emoji_bitmaps.c, CC-BY 4.0 Twitter/jdecked).

#include "emoji.h"

#include <stdint.h>
#include <string.h>

#include "pax_gfx.h"
#include "pax_text.h"
#include "shapes/pax_misc.h"

extern const uint32_t *const EMOJI_BITMAPS[];

#ifndef EMOJI_BITMAP_SIZE
#define EMOJI_BITMAP_SIZE 32
#endif

// EMOJI_SET, emoji_lookup_by_codepoint and utf8_decode moved to emoji_table.c
// (the pax-free part). This file keeps only the bitmap/drawing layer.

// ── Bitmap wrappers: one pax_buf_t per emoji, sharing its flash-resident pixel
// array. PAX treats image buffers as const for draw_image, so the cast-away
// const is safe — we never write into the buffer.
static pax_buf_t emoji_bufs[EMOJI_COUNT];
static bool      emoji_init_done = false;

void emoji_init(void) {
    if (emoji_init_done) return;
    for (int i = 0; i < EMOJI_COUNT; i++) {
        pax_buf_init(&emoji_bufs[i],
                     (void *)(uintptr_t)EMOJI_BITMAPS[i],
                     EMOJI_BITMAP_SIZE, EMOJI_BITMAP_SIZE,
                     PAX_BUF_32_8888ARGB);
    }
    emoji_init_done = true;
}

void emoji_draw(int idx, int cx, int cy, int radius, pax_buf_t *fb) {
    if (idx < 0 || idx >= EMOJI_COUNT) return;
    if (!emoji_init_done) emoji_init();
    float d = (float)(radius * 2);
    pax_draw_image_sized(fb, &emoji_bufs[idx],
                         (float)(cx - radius), (float)(cy - radius), d, d);
}

static int emoji_inline_diameter(float size) {
    int d = (int)(size * 1.1f);
    if (d < 12) d = 12;
    return d;
}

static int flush_text_run(pax_buf_t *fb, pax_col_t col, const pax_font_t *font,
                          float size, int x, int y, char *buf, int *blen) {
    if (*blen == 0) return 0;
    buf[*blen] = '\0';
    pax_vec2f sz = pax_text_size(font, size, buf);
    if (fb) pax_draw_text(fb, col, font, size, x, y, buf);
    *blen = 0;
    return (int)sz.x;
}

static int render_or_measure(pax_buf_t *fb, pax_col_t col, const pax_font_t *font,
                             float size, int x, int y, const char *text) {
    int dx = 0;
    int d  = emoji_inline_diameter(size);
    int r  = d / 2;

    char run[256];
    int  run_len = 0;

    int i = 0;
    while (text[i]) {
        uint32_t cp = 0;
        int adv = utf8_decode(&text[i], &cp);
        if (adv <= 0) {
            if (run_len < (int)sizeof(run) - 1) run[run_len++] = '?';
            i++;
            continue;
        }
        int idx = (cp >= 0x80) ? emoji_lookup_by_codepoint(cp) : -1;
        if (idx >= 0) {
            dx += flush_text_run(fb, col, font, size, x + dx, y, run, &run_len);
            int cx = x + dx + r;
            int cy = y + (int)(size * 0.5f);
            if (fb) emoji_draw(idx, cx, cy, r, fb);
            dx += d + 1;
            i += adv;
            continue;
        }
        if (cp >= 0x80) {
            if (run_len < (int)sizeof(run) - 1) run[run_len++] = '?';
            i += adv;
            continue;
        }
        if (run_len < (int)sizeof(run) - 1) run[run_len++] = (char)cp;
        i += adv;
    }
    dx += flush_text_run(fb, col, font, size, x + dx, y, run, &run_len);
    return dx;
}

int emoji_draw_text(pax_buf_t *fb, pax_col_t col, const pax_font_t *font,
                    float size, int x, int y, const char *text) {
    return render_or_measure(fb, col, font, size, x, y, text);
}

int emoji_measure_text(const pax_font_t *font, float size, const char *text) {
    return render_or_measure(NULL, 0, font, size, 0, 0, text);
}
