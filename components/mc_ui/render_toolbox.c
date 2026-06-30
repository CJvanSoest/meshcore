// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// VIEW_TOOLBOX — the Toolbox launcher tile registry: a short menu of LoRa
// diagnostic sub-tools, reached from the Settings "Toolbox" tile. The tile
// metadata (label/desc/enabled/target) is the single source of truth; the LVGL
// renderer in lvgl_ui.c reads enabled/target via the accessors below. The PAX
// painter was retired in the LVGL-only migration.

#include "app_config.h"
#include "render_internal.h"
#include "ui_state.h"

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
