// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
// SPDX-FileContributor: Ilias el Matani <hello@ilias.codes>
//
// Pure emoji table + UTF-8 decode. No pax / drawing dependency (that lives in
// emoji.c). Moved here verbatim from emoji.c so chat.c and other non-UI code
// can recognise emoji codepoints without depending on the graphics layer.

#include "emoji_table.h"

const emoji_entry_t EMOJI_SET[EMOJI_COUNT] = {
    {0x1F600, "\xF0\x9F\x98\x80", 4},  // grin
    {0x1F603, "\xF0\x9F\x98\x83", 4},  // smile
    {0x1F604, "\xF0\x9F\x98\x84", 4},  // laugh
    {0x1F602, "\xF0\x9F\x98\x82", 4},  // joy
    {0x1F923, "\xF0\x9F\xA4\xA3", 4},  // rofl
    {0x1F609, "\xF0\x9F\x98\x89", 4},  // wink
    {0x1F60A, "\xF0\x9F\x98\x8A", 4},  // blush
    {0x1F60D, "\xF0\x9F\x98\x8D", 4},  // hearteyes
    {0x1F618, "\xF0\x9F\x98\x98", 4},  // kiss
    {0x1F61B, "\xF0\x9F\x98\x9B", 4},  // tongue
    {0x1F60E, "\xF0\x9F\x98\x8E", 4},  // cool
    {0x1F914, "\xF0\x9F\xA4\x94", 4},  // thinking
    {0x1F60F, "\xF0\x9F\x98\x8F", 4},  // smirk
    {0x1F612, "\xF0\x9F\x98\x92", 4},  // unamused
    {0x1F62D, "\xF0\x9F\x98\xAD", 4},  // sob
    {0x1F622, "\xF0\x9F\x98\xA2", 4},  // cry
    {0x1F620, "\xF0\x9F\x98\xA0", 4},  // angry
    {0x1F621, "\xF0\x9F\x98\xA1", 4},  // rage
    {0x1F631, "\xF0\x9F\x98\xB1", 4},  // scream
    {0x1F633, "\xF0\x9F\x98\xB3", 4},  // flushed
    {0x1F61E, "\xF0\x9F\x98\x9E", 4},  // sad
    {0x1F634, "\xF0\x9F\x98\xB4", 4},  // sleep
    {0x1F605, "\xF0\x9F\x98\x85", 4},  // sweat
    {0x1F610, "\xF0\x9F\x98\x90", 4},  // neutral
    {0x1F44D, "\xF0\x9F\x91\x8D", 4},  // thumbsup
    {0x1F44E, "\xF0\x9F\x91\x8E", 4},  // thumbsdown
    {0x1F44C, "\xF0\x9F\x91\x8C", 4},  // okhand
    {0x1F44F, "\xF0\x9F\x91\x8F", 4},  // clap
    {0x1F64F, "\xF0\x9F\x99\x8F", 4},  // pray
    {0x1F4AA, "\xF0\x9F\x92\xAA", 4},  // muscle
    {0x1F44B, "\xF0\x9F\x91\x8B", 4},  // wave
    {0x1F44A, "\xF0\x9F\x91\x8A", 4},  // fist
    {0x2764, "\xE2\x9D\xA4", 3},       // heart
    {0x1F494, "\xF0\x9F\x92\x94", 4},  // brokenheart
    {0x1F525, "\xF0\x9F\x94\xA5", 4},  // fire
    {0x2B50, "\xE2\xAD\x90", 3},       // star
    {0x2728, "\xE2\x9C\xA8", 3},       // sparkles
    {0x1F389, "\xF0\x9F\x8E\x89", 4},  // party
    {0x2705, "\xE2\x9C\x85", 3},       // check
    {0x274C, "\xE2\x9D\x8C", 3},       // cross
};

int emoji_lookup_by_codepoint(uint32_t cp) {
    for (int i = 0; i < EMOJI_COUNT; i++) {
        if (EMOJI_SET[i].codepoint == cp) return i;
    }
    return -1;
}

int utf8_decode(const char* s, uint32_t* out_cp) {
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
        if (out_cp) *out_cp = ((uint32_t)(b0 & 0x0F) << 12) | ((uint32_t)(b1 & 0x3F) << 6) | (b2 & 0x3F);
        return 3;
    }
    if ((b0 & 0xF8) == 0xF0) {
        unsigned char b1 = (unsigned char)s[1], b2 = (unsigned char)s[2], b3 = (unsigned char)s[3];
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80) return -1;
        if (out_cp)
            *out_cp = ((uint32_t)(b0 & 0x07) << 18) | ((uint32_t)(b1 & 0x3F) << 12) | ((uint32_t)(b2 & 0x3F) << 6) |
                      (b3 & 0x3F);
        return 4;
    }
    return -1;
}
