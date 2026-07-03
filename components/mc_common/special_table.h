// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// Pure special-character table for the F5 ("blue cloud") picker. Reuses
// emoji_entry_t (codepoint + UTF-8 form + length) so the chat-input insert path
// is identical to the emoji picker. No pax / LVGL dependency: these are plain
// UTF-8 sequences, drawn by the UI as font glyphs (unlike emoji, which are
// bitmaps). The glyphs must exist in the extended Montserrat display font;
// see docs/features/Special-Characters.md.

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "emoji_table.h"  // emoji_entry_t

#define SPECIAL_COUNT 40

extern const emoji_entry_t SPECIAL_SET[SPECIAL_COUNT];

// True if the extended Montserrat display font (mc_fonts) has a glyph for `cp`:
// ASCII, Latin-1 Supplement, bullet, en/em dash and the euro sign. The UTF-8
// sanitiser uses this to keep displayable characters (umlauts, accents, °, €, …)
// instead of collapsing every non-emoji multi-byte sequence to '?'. Kept in
// lockstep with the ranges in scripts/gen_ext_fonts.sh.
bool special_font_covers(uint32_t cp);
