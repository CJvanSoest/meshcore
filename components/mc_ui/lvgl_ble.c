// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// VIEW_BLE_DEVICES.

#include <stdio.h>
#include "ble_companion.h"
#include "history.h"
#include "lvgl.h"
#include "lvgl_internal.h"
#include "lvgl_port.h"
#include "render.h"
#include "ui_state.h"

// ── VIEW_BLE_DEVICES ─────────────────────────────────────────────────────────
// BLE companion status + the list of bonded phones, opened from Settings >
// Bluetooth. Directly diagnoses "badge not visible in the app": the status
// block shows whether NimBLE actually came up and is advertising. The tail row
// clears all bonds (two-Enter confirm) to recover from a stale pairing. At most
// a handful of bonds exist (NimBLE store cap), so the flat list always fits.
void render_ble_devices_lvgl(void) {
    int       w   = (int)lvgl_port_width();
    int       h   = (int)lvgl_port_height();
    lv_obj_t* scr = begin_screen(COL_PAGER_BG);
    pt_reset();

    ble_status_t st;
    ble_companion_get_status(&st);
    ble_peer_t peers[8];
    int        np = ble_companion_bonded_peers(peers, 8);

    // Header
    add_rect(scr, 0, 0, w, ST_HEADER_H, COL_PAGER_BG);
    add_rect(scr, 0, ST_HEADER_H - 1, w, 1, COL_PAGER_ACCENT);
    add_label(scr, 12, (ST_HEADER_H - TXT_TAB) / 2, TXT_TAB, COL_PAGER_TEXT, "Bluetooth / Paired devices");

    int  x = 16;
    int  y = ST_HEADER_H + 10;
    char line[80];

    // ── Status block ──
    snprintf(line, sizeof(line), "Radio: %s", st.initialized ? "up" : "DOWN (init failed)");
    add_label(scr, x, y, TXT_BODY, st.initialized ? COL_GREEN : COL_RED, line);
    y += ST_ROW_H;
    snprintf(line, sizeof(line), "Advertising: %s", st.advertising ? "yes (discoverable)" : "no");
    add_label(scr, x, y, TXT_BODY, st.advertising ? COL_GREEN : COL_HINT, line);
    y += ST_ROW_H;
    snprintf(line, sizeof(line), "Connection: %s", st.connected ? "connected" : "none");
    add_label(scr, x, y, TXT_BODY, st.connected ? COL_GREEN : COL_HINT, line);
    y += ST_ROW_H + 4;

    // Divider + list count
    add_rect(scr, x, y, w - 2 * x, 1, COL_PAGER_ACCENT);
    y += 8;
    snprintf(line, sizeof(line), "Paired devices: %d", np);
    add_label(scr, x, y, TXT_SMALL, COL_HINT, line);
    y += TXT_SMALL + 6;

    int rw = w - 2 * x;
    // Device rows (display-only, but focus-highlighted for nav feedback)
    for (int i = 0; i < np; i++) {
        bool foc = (i == ble_devices_cursor);
        add_rect(scr, x, y, rw, ST_ROW_H, foc ? COL_PAGER_ACCENT : COL_PAGER_TILE);
        snprintf(line, sizeof(line), "%02X:%02X:%02X:%02X:%02X:%02X  (%s)", peers[i].addr[0], peers[i].addr[1],
                 peers[i].addr[2], peers[i].addr[3], peers[i].addr[4], peers[i].addr[5],
                 peers[i].addr_type == 0 ? "public" : "random");
        add_label(scr, x + 12, y + (ST_ROW_H - TXT_BODY) / 2, TXT_BODY, foc ? COL_HEADER : COL_PAGER_TEXT, line);
        y += ST_ROW_H + 4;
    }
    if (np == 0) {
        add_label(scr, x + 12, y, TXT_SMALL, COL_HINT, "(none yet -- pair from the MeshCore app)");
        y += ST_ROW_H;
    }

    // ── Clear-bonds action row (tail; focused when the cursor is past the list) ──
    bool afoc = (ble_devices_cursor >= np);
    add_rect(scr, x, y, rw, ST_ROW_H, afoc ? COL_PAGER_ACCENT : COL_PAGER_TILE);
    add_label(scr, x + 12, y + (ST_ROW_H - TXT_BODY) / 2, TXT_BODY, afoc ? COL_HEADER : COL_RED, "Clear all bonds");

    // ── Footer ──
    int fy = h - ST_FOOTER_H;
    add_rect(scr, 0, fy, w, ST_FOOTER_H, COL_HEADER);
    add_rect(scr, 0, fy, w, 1, COL_PAGER_ACCENT);
    int ty = fy + (ST_FOOTER_H - TXT_SMALL) / 2;
    if (ble_devices_confirm) {
        add_label(scr, 10, ty, TXT_SMALL, COL_RED, "Clear all bonds?  Enter = do it   ESC = cancel");
    } else {
        const char* hint = "WS: nav   Enter: clear bonds   ";
        add_label(scr, 10, ty, TXT_SMALL, COL_HINT, hint);
        add_back_hint(scr, 10 + text_w(hint, TXT_SMALL), ty, ": settings", TXT_SMALL);
    }
}
