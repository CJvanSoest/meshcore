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

// Overlays drawn on top of a base view by the dispatcher.
void render_qr_overlay(void);
void render_emoji_picker_overlay(void);
