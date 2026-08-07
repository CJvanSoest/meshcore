// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// LVGL-backed view renderers. render() in render.c calls lvgl_view_render() for
// the current view; each renderer rebuilds the LVGL screen from app state, so
// LVGL is purely the paint layer and input semantics live in input.c.

#pragma once

#include <stdbool.h>
#include <stdint.h>      // uint32_t (splash colour)
#include "app_config.h"  // app_view_t

// Rebuild + flush the LVGL screen for `v`.
void lvgl_view_render(app_view_t v);

// ── Boot splash ──────────────────────────────────────────────────────────────
// Incremental init readout drawn during app_main before the first view render.
// Colours are passed as 0xAARRGGBB (the app's COL_* palette) so main.c does not
// need lvgl.h on its include path. lvgl_port_init() must have run first.
void lvgl_splash_begin(const char* title, const char* subtitle);
void lvgl_splash_line(uint32_t argb, const char* text);
