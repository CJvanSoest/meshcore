// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// Host-side test for the F5 special-character bank in
// components/mc_common/special_table.c.
//
// Links against the SAME translation units the firmware ships (special_table.c
// + emoji_table.c, the latter for utf8_decode). A wrong UTF-8 byte, a bad
// length, or a duplicate codepoint would insert a broken sequence into an
// outgoing message, so it should make CI red before it can merge.
//
// Build (see tests/Makefile):
//     gcc test_special_table.c ../components/mc_common/special_table.c
//         ../components/mc_common/emoji_table.c -o test_special_table
//
// Exit 0 on pass, 1 on any mismatch.

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "emoji_table.h"
#include "special_table.h"

static int failures = 0;

#define EXPECT(cond, fmt, ...)                                                           \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            fprintf(stderr, "FAIL %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
            failures++;                                                                  \
        }                                                                                \
    } while (0)

int main(void) {
    // The picker grid is a fixed 4x10; the count must stay in lockstep with the
    // UI's paging math (cols=4, vis_rows=5, two pages of 20).
    EXPECT(SPECIAL_COUNT == 40, "expected 40 entries, got %d", SPECIAL_COUNT);

    for (int i = 0; i < SPECIAL_COUNT; i++) {
        const emoji_entry_t* e = &SPECIAL_SET[i];

        // utf8_len must match the actual NUL-terminated length (no embedded NUL,
        // no trailing garbage), and fit the shared buffer bound.
        EXPECT(e->utf8 != NULL, "entry %d: NULL utf8", i);
        EXPECT(e->utf8_len == strlen(e->utf8), "entry %d (U+%04X): utf8_len %u != strlen %zu", i, e->codepoint,
               e->utf8_len, strlen(e->utf8));
        EXPECT(e->utf8_len >= 1 && e->utf8_len <= EMOJI_UTF8_MAX - 1, "entry %d: utf8_len %u out of range", i,
               e->utf8_len);

        // Decoding the stored bytes must reproduce the declared codepoint, and
        // consume exactly utf8_len bytes.
        uint32_t cp       = 0;
        int      consumed = utf8_decode(e->utf8, &cp);
        EXPECT(consumed == e->utf8_len, "entry %d (U+%04X): decode consumed %d, want %u", i, e->codepoint, consumed,
               e->utf8_len);
        EXPECT(cp == e->codepoint, "entry %d: decoded U+%04X != declared U+%04X", i, cp, e->codepoint);
    }

    // No duplicate codepoints — a dup wastes a grid slot and confuses the user.
    for (int i = 0; i < SPECIAL_COUNT; i++) {
        for (int j = i + 1; j < SPECIAL_COUNT; j++) {
            EXPECT(SPECIAL_SET[i].codepoint != SPECIAL_SET[j].codepoint, "duplicate codepoint U+%04X at %d and %d",
                   SPECIAL_SET[i].codepoint, i, j);
        }
    }

    // Spot-check the reporter's headline characters are present and correct.
    EXPECT(SPECIAL_SET[0].codepoint == 0x00E4, "slot 0 should be a-umlaut");
    EXPECT(strcmp(SPECIAL_SET[0].utf8, "\xC3\xA4") == 0, "a-umlaut bytes");
    EXPECT(SPECIAL_SET[3].codepoint == 0x00DF, "slot 3 should be sharp-s");

    // Every character in the picker must be one the display font can draw —
    // otherwise the UTF-8 sanitiser would collapse it to '?' on receive.
    for (int i = 0; i < SPECIAL_COUNT; i++) {
        EXPECT(special_font_covers(SPECIAL_SET[i].codepoint), "font must cover slot %d (U+%04X)", i,
               SPECIAL_SET[i].codepoint);
    }
    // ASCII stays covered; out-of-range codepoints (Latin Extended-A, emoji, CJK)
    // must not, so genuinely undrawable text still degrades to '?'.
    EXPECT(special_font_covers('A'), "ASCII must be covered");
    EXPECT(!special_font_covers(0x0141), "Latin Extended-A (L-stroke) must not be covered");
    EXPECT(!special_font_covers(0x1F600), "emoji codepoint must not be font-covered");
    EXPECT(!special_font_covers(0x4E00), "CJK must not be covered");

    if (failures) {
        fprintf(stderr, "%d assertion(s) failed\n", failures);
        return 1;
    }
    printf("test_special_table: all checks passed (%d entries)\n", SPECIAL_COUNT);
    return 0;
}
