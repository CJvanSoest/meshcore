// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// Pure emoji table + UTF-8 decode. No pax / drawing dependency (that lives in
// emoji.c). Moved here verbatim from emoji.c so chat.c and other non-UI code
// can recognise emoji codepoints without depending on the graphics layer.

#include "emoji_table.h"

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
