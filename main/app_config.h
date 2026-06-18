// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#pragma once

// Shared sizing constants used across modules (chat, history, ...).
#define MAX_CHAT_MSGS 50
#define MAX_MSG_TEXT  172

// Hand-maintained label for the tanmatsu-radio C6 firmware currently on
// the badge. v3.1.1+ exposes the app version at runtime via the system
// protocol's get-information command, so this string is only the fallback
// shown when the live query fails. Bump it whenever you re-flash the C6.
#define TANMATSU_RADIO_FW_LABEL "v3.2.0"

// Top-level tab/view selection. Lives here because both render, input, and
// rx_task need to read it.
typedef enum {
    VIEW_SETTINGS = 0,
    VIEW_NODES    = 1,
    VIEW_CHAT     = 2,   // DM conversations
    VIEW_CHANNEL  = 3,   // public channel (GRP_TXT)
    VIEW_HOME     = 4,   // tile-grid landing screen (Pager-style)
    VIEW_ABOUT    = 5,   // version / authors / credits / license
    VIEW_MAP      = 6,   // OSM tile-based map + live GPS overlay
    VIEW_COUNT    = 7,
    // Number of views that appear in the legacy top tab-bar. VIEW_HOME,
    // VIEW_ABOUT and VIEW_MAP have their own headers, so the tab-bar
    // still iterates only the four classic views (0..3).
    VIEW_TAB_COUNT = 4,
} app_view_t;

extern app_view_t current_view;
