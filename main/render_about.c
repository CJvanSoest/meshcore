// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// VIEW_ABOUT — version, authors, credits, license. Reached via the About
// tile on the home screen; ESC returns to home.

#include "render.h"
#include "render_internal.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"

#include "app_config.h"

#define ABOUT_HEADER_H 50
#define ABOUT_FOOTER_H 38

typedef struct {
    pax_col_t   col;
    int         font_size;
    const char *text;
    int         space_after;  // extra pixels of vertical breathing room after
} about_line_t;

static void render_about_header(int w) {
    pax_simple_rect(&fb, COL_PAGER_BG,     0, 0, w, ABOUT_HEADER_H);
    pax_simple_rect(&fb, COL_PAGER_ACCENT, 0, ABOUT_HEADER_H - 1, w, 1);
    int ty = (ABOUT_HEADER_H - TXT_TAB) / 2;
    pax_draw_text(&fb, COL_PAGER_TEXT, FONT, TXT_TAB, 12, ty, "About");
}

void render_about(void) {
    int w = (int)pax_buf_get_width(&fb);
    int h = (int)pax_buf_get_height(&fb);

    pax_background(&fb, COL_PAGER_BG);
    render_about_header(w);

    // Pull live build metadata from the ESP-IDF app description (set at
    // link-time from git describe / build time).
    const esp_app_desc_t *desc = esp_app_get_description();
    const char *ver       = (desc && desc->version[0])      ? desc->version      : "?";
    const char *built_d   = (desc && desc->date[0])         ? desc->date         : "?";
    const char *built_t   = (desc && desc->time[0])         ? desc->time         : "";

    char ver_line[80];
    snprintf(ver_line, sizeof(ver_line), "MeshCore  %s", ver);
    char build_line[96];
    snprintf(build_line, sizeof(build_line), "Built %s  %s", built_d, built_t);

    // Static content: only `ver_line` + `build_line` vary at boot. The rest is
    // attribution text per the LICENSE + memory's upstream-attribution rule
    // ("Nicolai" / "Nicolai Electronics" in public-facing strings).
    about_line_t lines[] = {
        { COL_PAGER_ACCENT, TXT_TITLE, ver_line,                                 6  },
        { COL_GRAY,         TXT_SMALL, build_line,                               6  },
        { COL_AMBER,        TXT_SMALL, "Community app -- not the official "
                                       "MeshCore app",                          22 },

        { COL_PAGER_TEXT,   TXT_BODY,  "Author",                                 4  },
        { COL_GRAY,         TXT_SMALL, "CJ van Soest (CJvS)",                   16 },

        { COL_PAGER_TEXT,   TXT_BODY,  "Built on",                               4  },
        { COL_GRAY,         TXT_SMALL, "MeshCore  by  Ripple Radios",            2 },
        { COL_GRAY,         TXT_SMALL, "Tanmatsu  by  Nicolai Electronics",     16 },

        { COL_PAGER_TEXT,   TXT_BODY,  "License",                                4 },
        { COL_GRAY,         TXT_SMALL, "MIT (see LICENSE in the repo)",         16 },

        { COL_PAGER_TEXT,   TXT_BODY,  "Source",                                 4 },
        { COL_GRAY,         TXT_SMALL, "github.com/CJvanSoest/meshcore",         6 },

        { COL_PAGER_TEXT,   TXT_BODY,  "Issues / questions",                     4 },
        { COL_GRAY,         TXT_SMALL,
            "github.com/CJvanSoest/meshcore/issues",                             0 },
    };
    const int n_lines = (int)(sizeof(lines) / sizeof(lines[0]));

    int x = 28;
    int y = ABOUT_HEADER_H + 24;
    for (int i = 0; i < n_lines; i++) {
        pax_draw_text(&fb, lines[i].col, FONT, lines[i].font_size, x, y, lines[i].text);
        y += lines[i].font_size + 4 + lines[i].space_after;
    }

    int fy = h - ABOUT_FOOTER_H;
    pax_simple_rect(&fb, COL_HEADER,       0, fy, w, ABOUT_FOOTER_H);
    pax_simple_rect(&fb, COL_PAGER_ACCENT, 0, fy, w, 1);
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 10,
                  fy + (ABOUT_FOOTER_H - TXT_SMALL) / 2,
                  "ESC: home");
}
