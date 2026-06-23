// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#pragma once

// Cross-file declarations between render.c (dispatcher + tab-bar) and the
// per-view render_*.c files. Not part of the public render API — public
// callers use render.h.

#include "chat.h"      // chat_msg_t (render_msg_list signature)
#include "ui_state.h"  // app_view_t, field_t

// Top header strip; called at the start of every full-view render.
void render_tab_bar(void);

// Shared chat-message ring renderer used by both DM and channel views.
// Caller must hold the ring's mutex.
void render_msg_list(int w, int list_y0, int list_h, chat_msg_t* msgs, int head, int count, int* scroll_p);

// Per-view entry points dispatched by render() in render.c.
void render_settings(void);
void render_nodes(void);
void render_chat(void);
void render_channel(void);
void render_home(void);
void render_about(void);
void render_map(void);
void render_toolbox(void);           // Toolbox launcher (sub-tool menu)
void render_toolbox_log(void);       // live packet log (hex / dissector)
void render_toolbox_coverage(void);  // repeater coverage test

// Dump the current diag ring to a timestamped CSV under /sd/meshcore/log/ and
// raise a result toast. Called from the packet-log key handler ('E'). Safe to
// call from the UI task; takes a fresh ring snapshot internally.
void toolbox_log_export_sd(void);

// VIEW_HOME tile-grid API used by input.c to translate tile-Enter into a
// view switch + optional side-effect. home_tile_target() returns VIEW_HOME
// for placeholder ("soon") tiles so input can no-op on them.
typedef enum {
    HOME_ACTION_NONE = 0,
    HOME_ACTION_OPEN_QR,      // QR-tile: open QR overlay, stay rooted at home
    HOME_ACTION_OPEN_ADVERT,  // Advert-tile: drill into Settings -> Advert
} home_action_t;

int           home_tile_count(void);
app_view_t    home_tile_target(int idx);
home_action_t home_tile_action(int idx);

// VIEW_TOOLBOX launcher: list of diagnostic sub-tools. Disabled tiles ("soon")
// report enabled=false so input no-ops on them.
int        toolbox_tile_count(void);
bool       toolbox_tile_enabled(int idx);
app_view_t toolbox_tile_target(int idx);

// Settings drilldown: the Settings view is a two-level menu — a list of
// category cards, then a drilled-in view that only shows the fields belonging
// to one category. These helpers expose the category table (defined in
// render_settings.c) so input.c can clamp the field cursor + drive nav.
int         settings_category_count(void);
// Visible-only views: skips categories with hidden_from_grid set. Grid
// rendering + grid cursor nav use these so the hidden Advert category
// doesn't show up as a Settings tile but is still reachable via the
// Home -> Advert tile.
int         settings_visible_category_count(void);
int         settings_visible_category_real_idx(int slot);
void        settings_category_bounds(int cat, int* first_field, int* last_field);
const char* settings_category_title(int cat);
int         settings_category_for_field(int f);
// External (non-drilldown) categories switch straight to a top-level view.
// Returns true and fills *out_view when category `cat` is external.
bool        settings_category_is_external(int cat, app_view_t* out_view);

// Persist field `f` to NVS using the registry-defined save_*() (defined in
// render_settings.c's s_fields[]). Fields without a dedicated save_*()
// fall back to save_lora_config(). Replaces input.c's old persist_field_change.
void field_save(field_t f);

// Overlays drawn on top of a base view by the dispatcher.
void render_qr_overlay(void);
void render_emoji_picker_overlay(void);
