// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>

// Look up current lora_cfg.bandwidth in BW_OPTIONS; returns its index, or 7
// (125 kHz, the MeshCore default) if the value doesn't match any preset.
int bw_index(void);

// Settings-tab field stepper (delta = +1 or -1). Used by handle_nav / handle_key.
void field_adjust(int field, int delta);

// BSP navigation key (arrow / ESC / F1 / RETURN).
void handle_nav(uint32_t key);

// Printable / control character from the keyboard. `utf8` is the BSP event's
// UTF-8 string (may be NULL); it carries AltGr / dead-key characters that have
// no ASCII form, so text-input paths can insert umlauts/accents directly.
void handle_key(char c, const char* utf8);
