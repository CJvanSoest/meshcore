// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "history_trim.h"

uint32_t history_rec_disk_size(uint16_t plain_len) {
    // padded = ((plain_len + 16) / 16) * 16, i.e. round up to the next block
    // *after* the plaintext so there is always at least one pad byte.
    uint32_t padded = ((uint32_t)plain_len / 16u + 1u) * 16u;
    return HISTORY_REC_HDR_SIZE + padded;
}

int history_trim_first_kept(const uint32_t* sizes, int count, uint32_t cap) {
    if (count <= 0) return 0;
    uint32_t acc  = 0;
    int      kept = 0;
    // Grow the kept tail from the newest record backwards until the next
    // (older) record would overflow the cap.
    for (int i = count - 1; i >= 0; i--) {
        uint32_t next = acc + sizes[i];
        if (next > cap && kept > 0) break;
        acc = next;
        kept++;
    }
    if (kept == 0) kept = 1;  // always keep the newest record
    return count - kept;
}
