// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "render_settings_icons.h"

#include <math.h>

#include "pax_gfx.h"

#include "render.h"   // fb (shared draw buffer)

static void cat_icon_identity(int cx, int cy, int sz, pax_col_t col) {
    pax_outline_circle(&fb, col, cx, cy - sz / 6, sz / 4);  // head
    pax_outline_circle(&fb, col, cx, cy + sz / 3, sz / 2);  // shoulders (cropped)
}

// Hexagonal-ish "regulatory" badge -- six straight segments instead of
// a real outline_polygon so the line weight matches the rest of the set.
static void cat_icon_regulatory(int cx, int cy, int sz, pax_col_t col) {
    int s = sz / 2;
    pax_simple_line(&fb, col, cx,     cy - s, cx + s, cy - s / 3);
    pax_simple_line(&fb, col, cx + s, cy - s / 3, cx + s, cy + s / 3);
    pax_simple_line(&fb, col, cx + s, cy + s / 3, cx,     cy + s);
    pax_simple_line(&fb, col, cx,     cy + s, cx - s, cy + s / 3);
    pax_simple_line(&fb, col, cx - s, cy + s / 3, cx - s, cy - s / 3);
    pax_simple_line(&fb, col, cx - s, cy - s / 3, cx,     cy - s);
}

// Three concentric rings = a node radiating into the mesh.
static void cat_icon_radio(int cx, int cy, int sz, pax_col_t col) {
    int q = sz / 2;
    pax_outline_circle(&fb, col, cx, cy, q / 4);
    pax_outline_hollow_circle(&fb, col, cx, cy, q / 2 - 2, q / 2);
    pax_outline_hollow_circle(&fb, col, cx, cy, q     - 2, q);
}

// Antenna mast + tripod base + two outward radio waves at the tip.
static void cat_icon_advert(int cx, int cy, int sz, pax_col_t col) {
    int half = sz / 2;
    int top  = cy - half;
    int base = cy + half * 3 / 4;
    pax_simple_line(&fb, col, cx,           top,  cx,           base);
    pax_simple_line(&fb, col, cx,           base, cx - half / 2, base + half / 4);
    pax_simple_line(&fb, col, cx,           base, cx + half / 2, base + half / 4);
    pax_simple_line(&fb, col, cx - half / 3, top + half / 8, cx + half / 3, top + half / 8);
    pax_outline_hollow_circle(&fb, col, cx + half / 6, top + half / 6, half / 3 - 2, half / 3);
    pax_outline_hollow_circle(&fb, col, cx + half / 6, top + half / 6, half / 2 - 2, half / 2);
}

// Three small nodes connected by lines -- a stylised mesh triangle.
static void cat_icon_network(int cx, int cy, int sz, pax_col_t col) {
    int s = sz / 3;
    pax_simple_circle(&fb, col, cx,      cy - s, sz / 12);
    pax_simple_circle(&fb, col, cx - s,  cy + s, sz / 12);
    pax_simple_circle(&fb, col, cx + s,  cy + s, sz / 12);
    pax_simple_line  (&fb, col, cx,      cy - s, cx - s, cy + s);
    pax_simple_line  (&fb, col, cx,      cy - s, cx + s, cy + s);
    pax_simple_line  (&fb, col, cx - s,  cy + s, cx + s, cy + s);
}

// Map pin: tear-drop outline with a small dot in the bell.
static void cat_icon_region(int cx, int cy, int sz, pax_col_t col) {
    int r = sz / 3;
    pax_outline_circle(&fb, col, cx, cy - r / 2, r);
    pax_simple_line(&fb, col, cx - r * 7 / 10, cy - r / 8, cx, cy + r * 5 / 4);
    pax_simple_line(&fb, col, cx + r * 7 / 10, cy - r / 8, cx, cy + r * 5 / 4);
    pax_simple_circle(&fb, col, cx, cy - r / 2, r / 3);
}

// Speaker silhouette + outward sound wave arcs.
static void cat_icon_sounds(int cx, int cy, int sz, pax_col_t col) {
    int   half = sz / 2;
    int   s    = half * 2 / 3;
    pax_simple_line(&fb, col, cx - s / 2, cy - s / 4, cx - s / 2, cy + s / 4);
    pax_simple_line(&fb, col, cx - s / 2, cy - s / 4, cx,         cy - s / 2);
    pax_simple_line(&fb, col, cx,         cy - s / 2, cx,         cy + s / 2);
    pax_simple_line(&fb, col, cx,         cy + s / 2, cx - s / 2, cy + s / 4);
    float pi = 3.14159265f;
    pax_outline_arc(&fb, col, cx, cy, (float)(half * 5 / 10), -pi / 4.0f, pi / 4.0f);
    pax_outline_arc(&fb, col, cx, cy, (float)(half * 8 / 10), -pi / 4.0f, pi / 4.0f);
}

// Sun: small filled disc + 8 evenly-spaced rays.
static void cat_icon_brightness(int cx, int cy, int sz, pax_col_t col) {
    int r = sz / 5;
    pax_simple_circle(&fb, col, cx, cy, r);
    for (int a = 0; a < 8; a++) {
        float t  = (float)a * 3.14159f / 4.0f;
        float r0 = (float)r + sz / 14.0f;
        float r1 = (float)sz / 2.0f;
        pax_simple_line(&fb, col,
                        cx + r0 * cosf(t), cy + r0 * sinf(t),
                        cx + r1 * cosf(t), cy + r1 * sinf(t));
    }
}

// Index order MUST match s_categories[] in render_settings.c.
const cat_icon_fn settings_category_icons[] = {
    cat_icon_identity,
    cat_icon_regulatory,
    cat_icon_radio,
    cat_icon_advert,
    cat_icon_network,
    cat_icon_region,
    cat_icon_brightness,
    cat_icon_sounds,
};
const int settings_category_icons_count =
    (int)(sizeof(settings_category_icons) / sizeof(settings_category_icons[0]));
