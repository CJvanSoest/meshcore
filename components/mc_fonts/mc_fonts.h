// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// Extended Montserrat display faces covering Western/Central European Latin
// text (umlauts, accents, °, €, §, µ, ¿, ¡, «, », en/em dash) on top of ASCII.
// The stock LVGL built-in Montserrat faces only cover ASCII + ° + •, so
// received messages with umlauts and the F5 special-character picker would draw
// the missing-glyph box without these. Generated from the Montserrat-Medium.ttf
// LVGL bundles; see scripts/gen_ext_fonts.sh and docs/features/Special-Characters.md.

#pragma once

#include "lvgl.h"

LV_FONT_DECLARE(lv_font_montserrat_14_ext);
LV_FONT_DECLARE(lv_font_montserrat_16_ext);
LV_FONT_DECLARE(lv_font_montserrat_20_ext);
LV_FONT_DECLARE(lv_font_montserrat_22_ext);
LV_FONT_DECLARE(lv_font_montserrat_24_ext);
