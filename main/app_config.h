// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#pragma once

// Shared sizing constants used across modules (chat, history, ...).
#define MAX_CHAT_MSGS 50
#define MAX_MSG_TEXT  172

// Hand-maintained label for the tanmatsu-radio C6 firmware currently on
// the badge. v3.1.1 adds a system protocol with a get-information command
// that exposes the app version at runtime; until that client is wired up,
// bump this string whenever you re-flash the C6.
#define TANMATSU_RADIO_FW_LABEL "v3.1.1-1-gf919f91"

// Top-level tab/view selection. Lives here because both render, input, and
// rx_task need to read it.
typedef enum {
    VIEW_SETTINGS = 0,
    VIEW_NODES    = 1,
    VIEW_CHAT     = 2,   // DM conversations
    VIEW_CHANNEL  = 3,   // public channel (GRP_TXT)
    VIEW_COUNT    = 4,
} app_view_t;

extern app_view_t current_view;
