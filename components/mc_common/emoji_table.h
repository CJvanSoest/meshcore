// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
// SPDX-FileContributor: Ilias el Matani <hello@ilias.codes>
//
// Pure emoji table + UTF-8 helpers, with no pax / drawing dependency. Split
// out of emoji.h so non-UI code (e.g. chat.c, which only needs to recognise a
// codepoint) can use the lookup without dragging in the pax graphics stack.
// The drawing side (emoji_draw*, emoji_init) stays in emoji.h/emoji.c.

#pragma once

#include <stdint.h>

#define EMOJI_COUNT    8
#define EMOJI_UTF8_MAX 5  // longest sequence is 4 bytes + NUL

typedef struct {
    uint32_t    codepoint;   // Unicode scalar
    const char *utf8;        // NUL-terminated UTF-8 form (<=4 bytes)
    uint8_t     utf8_len;    // bytes in utf8 (without NUL)
} emoji_entry_t;

extern const emoji_entry_t EMOJI_SET[EMOJI_COUNT];

// Return index into EMOJI_SET for the given codepoint, or -1 if not in set.
int emoji_lookup_by_codepoint(uint32_t cp);

// Decode the UTF-8 sequence starting at `s`. Writes the codepoint to *out_cp
// and returns the number of bytes consumed (1..4) on success. Returns 0 if the
// first byte is NUL and -1 on a malformed sequence.
int utf8_decode(const char *s, uint32_t *out_cp);
