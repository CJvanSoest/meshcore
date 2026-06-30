// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// VIEW_HOME — tile-grid landing screen. The tile metadata (label/target/action)
// lives here as the single source of truth; the LVGL renderer in lvgl_ui.c reads
// the target/action via the accessors below so a tile-Enter opens the right view
// and triggers any post-open side-effect. The PAX painter was retired in the
// LVGL-only migration; only the non-rendering tile registry remains.

#include "app_config.h"
#include "render_internal.h"
#include "ui_state.h"

// ── Tile-grid geometry (kept in sync with lvgl_ui.c's home renderer) ─────────
#define HOME_TILE_COLS  3
#define HOME_TILE_ROWS  3
#define HOME_TILE_COUNT (HOME_TILE_COLS * HOME_TILE_ROWS)

// ── Tile definitions ─────────────────────────────────────────────────────────
typedef struct {
    const char*   label;
    app_view_t    target;  // view to open on Enter; VIEW_HOME = TBD
    home_action_t action;  // post-open side-effect (e.g. open QR overlay)
} home_tile_t;

static const home_tile_t home_tiles[HOME_TILE_COUNT] = {
    {"Nodes", VIEW_NODES, HOME_ACTION_NONE},
    {"DM", VIEW_CHAT, HOME_ACTION_NONE},
    {"Channel", VIEW_CHANNEL, HOME_ACTION_NONE},
    {"Map", VIEW_MAP, HOME_ACTION_NONE},
    {"Advert", VIEW_SETTINGS, HOME_ACTION_OPEN_ADVERT},
    {"Settings", VIEW_SETTINGS, HOME_ACTION_NONE},
    {"About", VIEW_ABOUT, HOME_ACTION_NONE},
    {"QR", VIEW_NODES, HOME_ACTION_OPEN_QR},
    {"Exit", VIEW_HOME, HOME_ACTION_EXIT},
};

// Expose the tile count + target/action lookup to input.c so Enter opens the
// right view and triggers any post-open side-effect.
int home_tile_count(void) {
    return HOME_TILE_COUNT;
}

app_view_t home_tile_target(int idx) {
    if (idx < 0 || idx >= HOME_TILE_COUNT) return VIEW_HOME;
    return home_tiles[idx].target;
}

home_action_t home_tile_action(int idx) {
    if (idx < 0 || idx >= HOME_TILE_COUNT) return HOME_ACTION_NONE;
    return home_tiles[idx].action;
}
