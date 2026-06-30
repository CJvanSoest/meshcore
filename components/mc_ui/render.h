// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#pragma once

#include <stddef.h>
#include <stdint.h>

// Shared palette + typography + layout constants for the LVGL views. (The PAX
// renderers that originally consumed these were retired in the LVGL-only
// migration; lvgl_ui.c maps COL_* to lv_color via mc_col() and TXT_* to the
// built-in Montserrat faces via mc_font().)

// ── Tokyo Night palette ──────────────────────────────────────────────────────
// COL_BLACK / COL_DARK are repurposed as background + panel surfaces.
#define COL_BLACK  0xFF1A1B26  // BG       — main background (deep indigo)
#define COL_HEADER 0xFF16161E  // HEADER   — tab bar + footer surface
#define COL_DARK   0xFF24283B  // PANEL    — row highlight / separators
#define COL_WHITE  0xFFC0CAF5  // TEXT     — body text (off-white)
#define COL_GRAY   0xFF565F89  // DIM      — secondary text
#define COL_HINT   COL_WHITE   // HINT     — footer hint text (bright/readable)
#define COL_ACCENT 0xFF7AA2F7  // BLUE     — selection / accent
#define COL_GREEN  0xFF9ECE6A  // GREEN    — ok / online
#define COL_YELLOW 0xFFE0AF68  // AMBER    — heading / warning
#define COL_RED    0xFFF7768E  // RED      — error / offline
#define COL_AMBER  COL_YELLOW  // alias for clarity in headings
#define COL_BLUE   COL_ACCENT  // alias for clarity in info rows
#define COL_PANEL  COL_DARK    // alias for clarity in surface use
#define COL_BG     COL_BLACK   // alias for clarity at root background

// ── LilyGo Pager palette (used by VIEW_HOME tile screen) ─────────────────────
// Borrowed from the LilyGo Pager MeshCore variant so the two devices share
// a visual identity on the home screen. The classic per-view screens
// (settings/chat/nodes/channel) keep the Tokyo Night palette above.
#define COL_PAGER_BG     0xFF0E141B  // window background
#define COL_PAGER_TILE   0xFF16161E  // unfocused tile surface
#define COL_PAGER_TEXT   0xFFC0C8D0  // body / label text
#define COL_PAGER_ACCENT 0xFFFAA61A  // focused tile + highlights (orange)

// ── Typography sizes (mapped onto the built-in Montserrat faces in lvgl_ui.c) ─
#define TXT_TITLE 24
#define TXT_TAB   22
#define TXT_BODY  20
#define TXT_SMALL 16
#define TXT_TINY  13

// ── Layout constants ─────────────────────────────────────────────────────────
#define TAB_BAR_H    44
#define FOOTER_H     28
#define CHAT_ROW_H   44
#define CHAT_Y0      (TAB_BAR_H + 4)
#define CHAT_INPUT_H 36

// ── Display globals (initialised in app_main, handed to lvgl_port_init) ───────
extern size_t display_h_res;
extern size_t display_v_res;

// Top-level dispatcher: paints the current view through LVGL.
void render(void);
