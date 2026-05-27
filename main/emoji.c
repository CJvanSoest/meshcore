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

const emoji_entry_t EMOJI_SET[EMOJI_COUNT] = {
    { 0x1F600, "\xF0\x9F\x98\x80", 4 },  // grin
    { 0x1F603, "\xF0\x9F\x98\x83", 4 },  // smile
    { 0x1F609, "\xF0\x9F\x98\x89", 4 },  // wink
    { 0x1F60A, "\xF0\x9F\x98\x8A", 4 },  // blush
    { 0x1F60E, "\xF0\x9F\x98\x8E", 4 },  // cool
    { 0x1F61B, "\xF0\x9F\x98\x9B", 4 },  // tongue
    { 0x1F622, "\xF0\x9F\x98\xA2", 4 },  // cry
    { 0x1F621, "\xF0\x9F\x98\xA1", 4 },  // angry
};

int emoji_lookup_by_codepoint(uint32_t cp) {
    for (int i = 0; i < EMOJI_COUNT; i++) {
        if (EMOJI_SET[i].codepoint == cp) return i;
    }
    return -1;
}

int utf8_decode(const char *s, uint32_t *out_cp) {
    unsigned char b0 = (unsigned char)s[0];
    if (b0 == 0) return 0;

    if (b0 < 0x80) {
        if (out_cp) *out_cp = b0;
        return 1;
    }
    if ((b0 & 0xE0) == 0xC0) {
        unsigned char b1 = (unsigned char)s[1];
        if ((b1 & 0xC0) != 0x80) return -1;
        if (out_cp) *out_cp = ((uint32_t)(b0 & 0x1F) << 6) | (b1 & 0x3F);
        return 2;
    }
    if ((b0 & 0xF0) == 0xE0) {
        unsigned char b1 = (unsigned char)s[1], b2 = (unsigned char)s[2];
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) return -1;
        if (out_cp) *out_cp = ((uint32_t)(b0 & 0x0F) << 12) |
                              ((uint32_t)(b1 & 0x3F) << 6)  |
                               (b2 & 0x3F);
        return 3;
    }
    if ((b0 & 0xF8) == 0xF0) {
        unsigned char b1 = (unsigned char)s[1], b2 = (unsigned char)s[2], b3 = (unsigned char)s[3];
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80) return -1;
        if (out_cp) *out_cp = ((uint32_t)(b0 & 0x07) << 18) |
                              ((uint32_t)(b1 & 0x3F) << 12) |
                              ((uint32_t)(b2 & 0x3F) << 6)  |
                               (b3 & 0x3F);
        return 4;
    }
    return -1;
}

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
