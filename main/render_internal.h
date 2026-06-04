// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#pragma once

// Cross-file declarations between render.c (dispatcher + tab-bar) and the
// per-view render_*.c files. Not part of the public render API — public
// callers use render.h.

#include "chat.h"  // chat_msg_t (render_msg_list signature)

// Top header strip; called at the start of every full-view render.
void render_tab_bar(void);

// Shared chat-message ring renderer used by both DM and channel views.
// Caller must hold the ring's mutex.
void render_msg_list(int w, int list_y0, int list_h, chat_msg_t *msgs,
                     int head, int count, int *scroll_p);

// Per-view entry points dispatched by render() in render.c.
void render_settings(void);
void render_nodes(void);
void render_chat(void);
void render_channel(void);
void render_home(void);
void render_about(void);

// VIEW_HOME tile-grid API used by input.c to translate tile-Enter into a
// view switch + optional side-effect. home_tile_target() returns VIEW_HOME
// for placeholder ("soon") tiles so input can no-op on them.
typedef enum {
    HOME_ACTION_NONE = 0,
    HOME_ACTION_OPEN_QR,        // QR-tile: open QR overlay, stay rooted at home
    HOME_ACTION_SEND_ADVERT,    // Advert-tile: send flood advert + show toast
} home_action_t;

int           home_tile_count(void);
app_view_t    home_tile_target(int idx);
home_action_t home_tile_action(int idx);

// Settings drilldown: the Settings view is a two-level menu — a list of
// category cards, then a drilled-in view that only shows the fields belonging
// to one category. These helpers expose the category table (defined in
// render_settings.c) so input.c can clamp the field cursor + drive nav.
int         settings_category_count(void);
void        settings_category_bounds(int cat, int *first_field, int *last_field);
const char *settings_category_title(int cat);
int         settings_category_for_field(int f);

// Overlays drawn on top of a base view by the dispatcher.
void render_qr_overlay(void);
void render_emoji_picker_overlay(void);
