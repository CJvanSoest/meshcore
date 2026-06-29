// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// LVGL 9 display glue for the migration. LVGL coexists with pax-gfx on this
// branch: per frame, either a PAX renderer or an LVGL screen drives the panel,
// never both. This module owns the lv_display + flush path (a single full-frame
// blit, software-rotated to match the known-good PAX orientation) and the tick.
//
// Deliberate design choice: there is NO LVGL input device. The app's existing
// input.c state machine (handle_nav / handle_key) stays the single source of
// truth; LVGL is a pure renderer that rebuilds its widget tree from that state
// each frame. This keeps every key semantic identical to the PAX build and
// avoids a second consumer racing the one BSP input queue.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "bsp/display.h"
#include "esp_err.h"

// Bring LVGL up on the panel. Pass the *native* panel parameters exactly as
// reported by bsp_display_get_parameters / bsp_display_get_default_rotation in
// main.c (e.g. 480x800, 565RGB, little-endian, rotation 270). The logical
// drawing surface LVGL exposes is the rotation-corrected size (e.g. 800x480),
// matching the PAX framebuffer the views were tuned against.
esp_err_t lvgl_port_init(size_t native_w, size_t native_h, bsp_display_color_format_t fmt,
                         bsp_display_endianness_t endian, bsp_display_rotation_t rot);

// True once lvgl_port_init has succeeded.
bool lvgl_port_ready(void);

// Logical (rotation-corrected) resolution LVGL draws in. 0 before init.
size_t lvgl_port_width(void);
size_t lvgl_port_height(void);

// Synchronously render any pending invalidations of the active LVGL screen and
// flush them to the panel. Called by the render() dispatcher for LVGL views.
void lvgl_port_refresh_now(void);

// The persistent screen object LVGL views build into. Cleared + rebuilt each
// frame by the view renderers. Typed as void* so this header stays free of
// lvgl.h (main.c includes it without pulling LVGL onto its include path).
void* lvgl_port_screen(void);
