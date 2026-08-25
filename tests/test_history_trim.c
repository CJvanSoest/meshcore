// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// Host-side test for the pure record-geometry + trim math in
// components/mc_domain/history_trim.c. history.c walks the on-disk file and
// rewrites it, but the two decisions that decide what survives a cap — the
// on-disk size of a record and how many oldest records to drop — live here so
// they can be checked without an SD card or mbedtls.
//
// Build (see tests/Makefile):
//     gcc test_history_trim.c ../components/mc_domain/history_trim.c -o test_history_trim
//
// Exit 0 on pass, 1 on any mismatch.

#include <stdint.h>
#include <stdio.h>
#include "history_trim.h"

static int failures = 0;

#define EXPECT(cond, fmt, ...)                                                           \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            fprintf(stderr, "FAIL %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
            failures++;                                                                  \
        }                                                                                \
    } while (0)

int main(void) {
    // ── history_rec_disk_size: 28-byte header + plaintext padded to the next
    //    16-byte block, always at least one pad byte. ──────────────────────────
    EXPECT(history_rec_disk_size(1) == 44, "len 1 -> %u", history_rec_disk_size(1));
    EXPECT(history_rec_disk_size(15) == 44, "len 15 -> %u", history_rec_disk_size(15));
    EXPECT(history_rec_disk_size(16) == 60, "len 16 -> %u", history_rec_disk_size(16));  // full extra block
    EXPECT(history_rec_disk_size(17) == 60, "len 17 -> %u", history_rec_disk_size(17));
    EXPECT(history_rec_disk_size(172) == 204, "len 172 -> %u", history_rec_disk_size(172));  // MAX_MSG_TEXT

    // ── history_trim_first_kept: keep the newest tail within cap ──────────────
    const uint32_t four[] = {44, 44, 44, 44};

    // Everything fits -> drop nothing.
    EXPECT(history_trim_first_kept(four, 4, 1000) == 0, "all-fit");
    EXPECT(history_trim_first_kept(four, 4, 176) == 0, "exact-total-fit");  // 4*44 == 176

    // Empty / degenerate inputs.
    EXPECT(history_trim_first_kept(four, 0, 100) == 0, "count 0");
    EXPECT(history_trim_first_kept(NULL, 0, 100) == 0, "null/0");

    // cap=100 holds two 44-byte records (88), a third would overflow -> keep 2,
    // so the first kept index is 4-2 = 2.
    EXPECT(history_trim_first_kept(four, 4, 100) == 2, "drop-oldest-2 -> %d", history_trim_first_kept(four, 4, 100));

    // Exact boundary: two records of 50 sum to the cap -> keep both.
    const uint32_t two50[] = {50, 50};
    EXPECT(history_trim_first_kept(two50, 2, 100) == 0, "exact-boundary");

    // A single newest record larger than the whole cap is still kept (never
    // drop to zero); everything older is dropped.
    const uint32_t oversized[] = {44, 44, 300};
    EXPECT(history_trim_first_kept(oversized, 3, 100) == 2, "keep-oversized-newest -> %d",
           history_trim_first_kept(oversized, 3, 100));

    // Single record, generous cap.
    const uint32_t one[] = {44};
    EXPECT(history_trim_first_kept(one, 1, 1000) == 0, "single-fits");
    EXPECT(history_trim_first_kept(one, 1, 10) == 0, "single-oversized-kept");

    if (failures == 0)
        printf("test_history_trim: OK\n");
    else
        printf("test_history_trim: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
