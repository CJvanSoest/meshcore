// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
// SPDX-FileContributor: Ilias el Matani <hello@ilias.codes>

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
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

// The emoji table, codepoint lookup and UTF-8 decode are the pax-free part and
// live in emoji_table.h so non-UI code can use them. This header adds the
// drawing layer on top.
#include "emoji_table.h"

// Wrap the embedded Twemoji bitmaps into pax_buf_t for pax_draw_image_sized.
// Idempotent — safe to call from any boot path; first call does the work.
void emoji_init(void);

// Draw a single emoji (by index into EMOJI_SET) centered at (cx, cy) with the
// given radius. Caller is responsible for clipping/positioning.
void emoji_draw(int idx, int cx, int cy, int radius, pax_buf_t* fb);

// Walk `text` and draw it at (x, baseline_y) using `font`/`size`/`col` for plain
// runs and `emoji_draw` for recognized codepoints. Returns total advance in px.
// Emoji are drawn inline with diameter ≈ size * 1.1 (matches the visual weight
// of the surrounding glyphs).
int emoji_draw_text(pax_buf_t* fb, pax_col_t col, const pax_font_t* font, float size, int x, int y, const char* text);

// Measure the would-be advance width of `text` with the same rules as above.
int emoji_measure_text(const pax_font_t* font, float size, const char* text);
