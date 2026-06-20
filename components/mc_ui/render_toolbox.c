// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// VIEW_TOOLBOX — the Toolbox launcher: a short menu of LoRa diagnostic
// sub-tools, reached from the Settings "Toolbox" tile. Sub-tools that are not
// built yet render dimmed with a "soon" tag. ESC returns to Settings.

#include <string.h>
#include "app_config.h"
#include "pax_gfx.h"
#include "pax_text.h"
#include "render.h"
#include "render_internal.h"
#include "ui_state.h"

#define TB_HEADER_H 50
#define TB_FOOTER_H 38
#define TB_ROW_H    64

typedef struct {
    const char* label;
    const char* desc;
    bool        enabled;  // false renders a dimmed "soon" placeholder
    app_view_t  target;   // inert when enabled == false
} toolbox_tile_t;

static const toolbox_tile_t toolbox_tiles[] = {
    {"Packet Log", "Live RX/TX frames, hex dump + dissector", true, VIEW_TOOLBOX_LOG},
    {"Coverage Test", "Ping repeaters, log reachability to SD", true, VIEW_TOOLBOX_COVERAGE},
};
#define TOOLBOX_TILE_COUNT ((int)(sizeof(toolbox_tiles) / sizeof(toolbox_tiles[0])))

int toolbox_tile_count(void) {
    return TOOLBOX_TILE_COUNT;
}

bool toolbox_tile_enabled(int idx) {
    return idx >= 0 && idx < TOOLBOX_TILE_COUNT && toolbox_tiles[idx].enabled;
}

app_view_t toolbox_tile_target(int idx) {
    return (idx >= 0 && idx < TOOLBOX_TILE_COUNT) ? toolbox_tiles[idx].target : VIEW_TOOLBOX;
}

static void toolbox_header(int w) {
    pax_simple_rect(&fb, COL_PAGER_BG, 0, 0, w, TB_HEADER_H);
    pax_simple_rect(&fb, COL_PAGER_ACCENT, 0, TB_HEADER_H - 1, w, 1);
    int ty = (TB_HEADER_H - TXT_TAB) / 2;
    pax_draw_text(&fb, COL_PAGER_TEXT, FONT, TXT_TAB, 12, ty, "Toolbox");
}

void render_toolbox(void) {
    int w = (int)pax_buf_get_width(&fb);
    int h = (int)pax_buf_get_height(&fb);

    pax_background(&fb, COL_PAGER_BG);
    toolbox_header(w);

    if (toolbox_cursor < 0) toolbox_cursor = 0;
    if (toolbox_cursor >= TOOLBOX_TILE_COUNT) toolbox_cursor = TOOLBOX_TILE_COUNT - 1;

    int x  = 20;
    int rw = w - 40;
    int y  = TB_HEADER_H + 20;
    for (int i = 0; i < TOOLBOX_TILE_COUNT; i++) {
        const toolbox_tile_t* t       = &toolbox_tiles[i];
        bool                  focused = (i == toolbox_cursor);

        pax_simple_rect(&fb, focused ? COL_PAGER_ACCENT : COL_PAGER_TILE, x, y, rw, TB_ROW_H);
        pax_col_t title_col = t->enabled ? (focused ? COL_HEADER : COL_PAGER_TEXT) : COL_GRAY;
        pax_col_t desc_col  = focused ? COL_HEADER : COL_GRAY;

        pax_draw_text(&fb, title_col, FONT, TXT_BODY, x + 16, y + 12, t->label);
        pax_draw_text(&fb, desc_col, FONT, TXT_SMALL, x + 16, y + 12 + TXT_BODY + 4, t->desc);

        if (!t->enabled) {
            const char* tag = "soon";
            pax_vec2f   sz  = pax_text_size(FONT, TXT_SMALL, tag);
            pax_draw_text(&fb, COL_AMBER, FONT, TXT_SMALL, x + rw - (int)sz.x - 16, y + (TB_ROW_H - TXT_SMALL) / 2,
                          tag);
        }
        y += TB_ROW_H + 14;
    }

    int fy = h - TB_FOOTER_H;
    pax_simple_rect(&fb, COL_HEADER, 0, fy, w, TB_FOOTER_H);
    pax_simple_rect(&fb, COL_PAGER_ACCENT, 0, fy, w, 1);
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 10, fy + (TB_FOOTER_H - TXT_SMALL) / 2,
                  "WS: nav   Enter: open   ESC: settings");
}
