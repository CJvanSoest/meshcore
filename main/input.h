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

// Printable / control character from the keyboard.
void handle_key(char c);
