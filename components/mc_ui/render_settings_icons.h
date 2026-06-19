// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// Tile-grid icons for the Settings category screen. The icon set lives in
// its own translation unit so render_settings.c stays focused on layout
// and field rendering. Index order MUST match s_categories[] in
// render_settings.c: any new category needs a matching slot in both.

#pragma once

#include "pax_types.h"

typedef void (*cat_icon_fn)(int cx, int cy, int sz, pax_col_t col);

extern const cat_icon_fn  settings_category_icons[];
extern const int          settings_category_icons_count;
