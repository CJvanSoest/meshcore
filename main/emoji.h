// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "pax_gfx.h"
#include "pax_text.h"

// Inline emoji rendering for MeshCore chat. Wire format is full UTF-8 (Unicode
// codepoints from the U+1F6xx Emoticons block) so messages round-trip 1:1 with
// other MeshCore clients (iPhone app, etc.). We draw the glyphs programmatically
// using PAX primitives — no font asset, no PSRAM cost.
//
// MVP set: 8 face emoji that cover most common reactions. Picker shows them as
// a 2x4 grid; chat-render scans text for recognized codepoints and draws the
// matching glyph instead of the Latin-1 '?' placeholder.

#define EMOJI_COUNT 8
#define EMOJI_UTF8_MAX 5  // longest sequence is 4 bytes + NUL

typedef struct {
    uint32_t    codepoint;          // Unicode scalar
    const char *utf8;               // NUL-terminated UTF-8 form (≤4 bytes)
    uint8_t     utf8_len;            // bytes in utf8 (without NUL)
} emoji_entry_t;

extern const emoji_entry_t EMOJI_SET[EMOJI_COUNT];

// Wrap the embedded Twemoji bitmaps into pax_buf_t for pax_draw_image_sized.
// Idempotent — safe to call from any boot path; first call does the work.
void emoji_init(void);

// Return index into EMOJI_SET for the given codepoint, or -1 if not in set.
int emoji_lookup_by_codepoint(uint32_t cp);

// Decode the UTF-8 sequence starting at `s`. Writes the codepoint to *out_cp
// and returns the number of bytes consumed (1..4) on success. Returns 0 if the
// first byte is NUL and -1 on a malformed sequence (no further bytes consumed).
int utf8_decode(const char *s, uint32_t *out_cp);

// Draw a single emoji (by index into EMOJI_SET) centered at (cx, cy) with the
// given radius. Caller is responsible for clipping/positioning.
void emoji_draw(int idx, int cx, int cy, int radius, pax_buf_t *fb);

// Walk `text` and draw it at (x, baseline_y) using `font`/`size`/`col` for plain
// runs and `emoji_draw` for recognized codepoints. Returns total advance in px.
// Emoji are drawn inline with diameter ≈ size * 1.1 (matches the visual weight
// of the surrounding glyphs).
int emoji_draw_text(pax_buf_t *fb, pax_col_t col, const pax_font_t *font,
                    float size, int x, int y, const char *text);

// Measure the would-be advance width of `text` with the same rules as above.
int emoji_measure_text(const pax_font_t *font, float size, const char *text);
