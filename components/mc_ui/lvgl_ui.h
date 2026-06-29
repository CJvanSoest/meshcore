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
#include "ui_state.h"  // app_view_t

// True when `v` is rendered through LVGL (migrated). False -> PAX path.
bool lvgl_view_active(app_view_t v);

// Rebuild + flush the LVGL screen for `v`. Precondition: lvgl_view_active(v).
void lvgl_view_render(app_view_t v);
