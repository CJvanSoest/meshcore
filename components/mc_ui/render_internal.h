// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#pragma once

// Cross-file declarations shared between the LVGL views (lvgl_ui.c), input.c,
// and the non-rendering registries that survived the LVGL-only migration
// (render_settings.c, render_home.c, render_toolbox.c, render_toolbox_log.c).
// Not part of the public render API — public callers use render.h.

#include <stddef.h>       // size_t (snapshot / format-detail signatures)
#include "diag.h"         // diag_entry_t (packet-log snapshot accessors)
#include "diag_decode.h"  // diag_decoded_t (packet-log snapshot accessors)
#include "ui_state.h"     // app_view_t, field_t

// Dump the current diag ring to a timestamped CSV under /sd/meshcore/log/ and
// raise a result toast. Called from the packet-log key handler ('E'). Safe to
// call from the UI task; takes a fresh ring snapshot internally.
void toolbox_log_export_sd(void);

// Packet-log snapshot accessors (the single-source snapshot + format logic live
// in render_toolbox_log.c). The LVGL view reuses them so the snapshot is not
// duplicated during the migration — mirrors how settings_field_label/value
// expose the field registry. Call toolbox_log_snapshot() first, then map
// newest-first indices with toolbox_log_snap_ri() against the captured head.
// Returns false if the PSRAM snapshot buffers are unavailable.
bool toolbox_log_snapshot(const diag_entry_t** out_snap, const diag_decoded_t** out_decoded, int* out_count,
                          int* out_head);
int  toolbox_log_snap_ri(int newest_idx);
void toolbox_log_format_detail(const diag_entry_t* e, const diag_decoded_t* d, char* out, size_t cap);

// VIEW_HOME tile-grid API used by input.c to translate tile-Enter into a
// view switch + optional side-effect. home_tile_target() returns VIEW_HOME
// for placeholder ("soon") tiles so input can no-op on them.
typedef enum {
    HOME_ACTION_NONE = 0,
    HOME_ACTION_OPEN_QR,      // QR-tile: open QR overlay, stay rooted at home
    HOME_ACTION_OPEN_ADVERT,  // Advert-tile: drill into Settings -> Advert
    HOME_ACTION_EXIT,         // Exit-tile: return to the BadgeVMS launcher
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

// Field registry accessors (table + value switch live in render_settings.c).
// The LVGL settings view reuses them so the per-field label/value formatting
// is not duplicated across the PAX + LVGL renderers during the migration.
const char* settings_field_label(field_t f);
void        settings_field_value(field_t f, char* out, size_t cap);
// Optional inline section header drawn above a field's drilldown row; NULL when
// the field has no header (only the Network/Region groups opt in).
const char* settings_section_above(field_t f);
