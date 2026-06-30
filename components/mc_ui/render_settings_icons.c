// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "render_settings_icons.h"
#include <math.h>
#include "pax_gfx.h"
#include "render.h"  // fb (shared draw buffer)

// ID card: a landscape card with a person silhouette on the left (head +
// shoulder arc) and three text lines on the right — the classic identity glyph.
static void cat_icon_identity(int cx, int cy, int sz, pax_col_t col) {
    int half = sz / 2;
    int x0 = cx - half, x1 = cx + half;      // card spans the full icon width
    int y0 = cy - sz / 3, y1 = cy + sz / 3;  // landscape card
    pax_simple_line(&fb, col, x0, y0, x1, y0);
    pax_simple_line(&fb, col, x1, y0, x1, y1);
    pax_simple_line(&fb, col, x1, y1, x0, y1);
    pax_simple_line(&fb, col, x0, y1, x0, y0);
    // person on the left third: head + shoulder arc (upper semicircle)
    int pcx   = cx - sz / 4;
    int headR = sz / 10;
    int headY = cy - sz / 10;
    pax_outline_circle(&fb, col, pcx, headY, headR);
    pax_outline_arc(&fb, col, pcx, headY + headR + sz / 16, sz / 8, -3.14159265f, 0.0f);
    // three text lines on the right
    int lx0 = cx + sz / 16, lx1 = x1 - sz / 10;
    int ly = cy - sz / 8;
    for (int i = 0; i < 3; i++) {
        pax_simple_line(&fb, col, lx0, ly, lx1, ly);
        ly += sz / 8;
    }
}

// Hexagonal-ish "regulatory" badge -- six straight segments instead of
// a real outline_polygon so the line weight matches the rest of the set.
static void cat_icon_regulatory(int cx, int cy, int sz, pax_col_t col) {
    int s = sz / 2;
    pax_simple_line(&fb, col, cx, cy - s, cx + s, cy - s / 3);
    pax_simple_line(&fb, col, cx + s, cy - s / 3, cx + s, cy + s / 3);
    pax_simple_line(&fb, col, cx + s, cy + s / 3, cx, cy + s);
    pax_simple_line(&fb, col, cx, cy + s, cx - s, cy + s / 3);
    pax_simple_line(&fb, col, cx - s, cy + s / 3, cx - s, cy - s / 3);
    pax_simple_line(&fb, col, cx - s, cy - s / 3, cx, cy - s);
}

// Radio set: a body box with a round speaker on the left, a tuning grille on
// the right, and a diagonal antenna rising from the top — the classic radio glyph.
static void cat_icon_radio(int cx, int cy, int sz, pax_col_t col) {
    int half = sz / 2;
    int x0 = cx - half, x1 = cx + half;
    int y0 = cy - sz / 6, y1 = cy + sz / 3;  // body sits in the lower portion
    // antenna: from the body's top-left up and to the right
    pax_simple_line(&fb, col, x0 + sz / 6, y0, cx + sz / 5, y0 - sz / 3);
    // body box
    pax_simple_line(&fb, col, x0, y0, x1, y0);
    pax_simple_line(&fb, col, x1, y0, x1, y1);
    pax_simple_line(&fb, col, x1, y1, x0, y1);
    pax_simple_line(&fb, col, x0, y1, x0, y0);
    // speaker dial (left)
    int bodyH = y1 - y0;
    int spR   = bodyH / 2 - sz / 16;
    int spcx  = x0 + bodyH / 2 + sz / 16;
    int spcy  = (y0 + y1) / 2;
    pax_outline_circle(&fb, col, spcx, spcy, spR);
    // tuning grille (right)
    int gx0 = spcx + spR + sz / 12, gx1 = x1 - sz / 10;
    int gy = y0 + bodyH / 4;
    for (int i = 0; i < 3; i++) {
        pax_simple_line(&fb, col, gx0, gy, gx1, gy);
        gy += bodyH / 4;
    }
}

// Antenna mast + tripod base + two outward radio waves at the tip.
static void cat_icon_advert(int cx, int cy, int sz, pax_col_t col) {
    int half = sz / 2;
    int top  = cy - half;
    int base = cy + half * 3 / 4;
    pax_simple_line(&fb, col, cx, top, cx, base);
    pax_simple_line(&fb, col, cx, base, cx - half / 2, base + half / 4);
    pax_simple_line(&fb, col, cx, base, cx + half / 2, base + half / 4);
    pax_simple_line(&fb, col, cx - half / 3, top + half / 8, cx + half / 3, top + half / 8);
    pax_outline_hollow_circle(&fb, col, cx + half / 6, top + half / 6, half / 3 - 2, half / 3);
    pax_outline_hollow_circle(&fb, col, cx + half / 6, top + half / 6, half / 2 - 2, half / 2);
}

// Three small nodes connected by lines -- a stylised mesh triangle.
static void cat_icon_network(int cx, int cy, int sz, pax_col_t col) {
    int s = sz / 3;
    pax_simple_circle(&fb, col, cx, cy - s, sz / 12);
    pax_simple_circle(&fb, col, cx - s, cy + s, sz / 12);
    pax_simple_circle(&fb, col, cx + s, cy + s, sz / 12);
    pax_simple_line(&fb, col, cx, cy - s, cx - s, cy + s);
    pax_simple_line(&fb, col, cx, cy - s, cx + s, cy + s);
    pax_simple_line(&fb, col, cx - s, cy + s, cx + s, cy + s);
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
    int half = sz / 2;
    int s    = half * 2 / 3;
    pax_simple_line(&fb, col, cx - s / 2, cy - s / 4, cx - s / 2, cy + s / 4);
    pax_simple_line(&fb, col, cx - s / 2, cy - s / 4, cx, cy - s / 2);
    pax_simple_line(&fb, col, cx, cy - s / 2, cx, cy + s / 2);
    pax_simple_line(&fb, col, cx, cy + s / 2, cx - s / 2, cy + s / 4);
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
        pax_simple_line(&fb, col, cx + r0 * cosf(t), cy + r0 * sinf(t), cx + r1 * cosf(t), cy + r1 * sinf(t));
    }
}

// Toolbox: a tool chest — body box with a lid line and a top handle arch.
static void cat_icon_toolbox(int cx, int cy, int sz, pax_col_t col) {
    int half = sz / 2;
    int bx0 = cx - half, bx1 = cx + half;
    int by0 = cy - half / 5, by1 = cy + half;
    pax_simple_line(&fb, col, bx0, by0, bx1, by0);                      // body: top
    pax_simple_line(&fb, col, bx1, by0, bx1, by1);                      // body: right
    pax_simple_line(&fb, col, bx1, by1, bx0, by1);                      // body: bottom
    pax_simple_line(&fb, col, bx0, by1, bx0, by0);                      // body: left
    pax_simple_line(&fb, col, bx0, cy + half / 4, bx1, cy + half / 4);  // lid seam
    int hx0 = cx - half / 3, hx1 = cx + half / 3, hy = cy - half / 2;
    pax_simple_line(&fb, col, hx0, by0, hx0, hy);  // handle: left post
    pax_simple_line(&fb, col, hx1, by0, hx1, hy);  // handle: right post
    pax_simple_line(&fb, col, hx0, hy, hx1, hy);   // handle: top
}

// Index order MUST match s_categories[] in render_settings.c.
const cat_icon_fn settings_category_icons[] = {
    cat_icon_identity, cat_icon_regulatory, cat_icon_radio,  cat_icon_advert,  cat_icon_network,
    cat_icon_region,   cat_icon_brightness, cat_icon_sounds, cat_icon_toolbox,
};
const int settings_category_icons_count = (int)(sizeof(settings_category_icons) / sizeof(settings_category_icons[0]));
