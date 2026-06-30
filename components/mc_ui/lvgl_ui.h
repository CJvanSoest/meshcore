// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// LVGL-backed view renderers. The render() dispatcher in render.c asks
// lvgl_view_active() whether the current view has been migrated; if so it calls
// lvgl_view_render() instead of the PAX render_*() path. Each renderer rebuilds
// the LVGL screen from current app state (the same globals the PAX views read),
// so input semantics are unchanged — LVGL is purely the paint layer.

#pragma once

#include <stdbool.h>
#include <stdint.h>      // uint32_t (splash colour)
#include "app_config.h"  // app_view_t

// True when `v` is rendered through LVGL. Every view is LVGL after the cleanup,
// so this returns true for all valid views (kept for the dispatcher contract).
bool lvgl_view_active(app_view_t v);

// Rebuild + flush the LVGL screen for `v`.
void lvgl_view_render(app_view_t v);

// ── Boot splash ──────────────────────────────────────────────────────────────
// Incremental init readout drawn during app_main before the first view render.
// Colours are passed as 0xAARRGGBB (the app's COL_* palette) so main.c does not
// need lvgl.h on its include path. lvgl_port_init() must have run first.
void lvgl_splash_begin(const char* title, const char* subtitle);
void lvgl_splash_line(uint32_t argb, const char* text);
