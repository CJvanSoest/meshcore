// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// VIEW_TOOLBOX.

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "app_config.h"
#include "appfs.h"
#include "backup.h"
#include "ble_companion.h"
#include "bsp/power.h"
#include "channels.h"
#include "chat.h"
#include "contacts.h"
#include "coverage.h"
#include "diag.h"
#include "diag_decode.h"
#include "emoji_table.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gps_task.h"
#include "history.h"
#include "identity.h"
#include "locfs.h"
#include "lvgl.h"
#include "lvgl_internal.h"
#include "lvgl_port.h"
#include "lvgl_ui.h"
#include "map.h"
#include "mc_fonts.h"  // extended Montserrat faces (umlauts, accents, °, €, …)
#include "nodes.h"
#include "nvs_flash.h"
#include "qrcodegen.h"
#include "radio.h"
#include "region_limits.h"
#include "render.h"  // COL_* palette + TXT_* sizes
#include "render_internal.h"
#include "settings_nvs.h"
#include "special_table.h"  // special_font_covers (draw umlauts instead of '?')
#include "ui_state.h"
#include "wifi_connection.h"

// ── VIEW_TOOLBOX ─────────────────────────────────────────────────────────────
// Port of render_toolbox.c. Tile metadata mirrors toolbox_tiles[] there.

#define TB_HEADER_H 50
#define TB_FOOTER_H 38
#define TB_ROW_H    64

typedef struct {
    const char* label;
    const char* desc;
    bool        enabled;
} tb_meta_t;

static const tb_meta_t tb_meta[] = {
    {"Packet Log", "Live RX/TX frames, hex dump + dissector", true},
    {"Coverage Test", "Ping repeaters, log reachability to SD", true},
    {"Storage Viewer", "NVS/SD usage + backup / restore", true},
};
#define TB_COUNT ((int)(sizeof(tb_meta) / sizeof(tb_meta[0])))

void render_toolbox_lvgl(void) {
    int       w   = (int)lvgl_port_width();
    int       h   = (int)lvgl_port_height();
    lv_obj_t* scr = begin_screen(COL_PAGER_BG);
    pt_reset();

    add_rect(scr, 0, 0, w, TB_HEADER_H, COL_PAGER_BG);
    add_rect(scr, 0, TB_HEADER_H - 1, w, 1, COL_PAGER_ACCENT);
    add_label(scr, 12, (TB_HEADER_H - TXT_TAB) / 2, TXT_TAB, COL_PAGER_TEXT, "Toolbox");

    if (toolbox_cursor < 0) toolbox_cursor = 0;
    if (toolbox_cursor >= TB_COUNT) toolbox_cursor = TB_COUNT - 1;

    int x  = 20;
    int rw = w - 40;
    int y  = TB_HEADER_H + 20;
    for (int i = 0; i < TB_COUNT; i++) {
        bool foc = (i == toolbox_cursor);
        add_rect(scr, x, y, rw, TB_ROW_H, foc ? COL_PAGER_ACCENT : COL_PAGER_TILE);
        uint32_t title_col = tb_meta[i].enabled ? (foc ? COL_HEADER : COL_PAGER_TEXT) : COL_GRAY;
        uint32_t desc_col  = foc ? COL_HEADER : COL_GRAY;
        add_label(scr, x + 16, y + 12, TXT_BODY, title_col, tb_meta[i].label);
        add_label(scr, x + 16, y + 12 + TXT_BODY + 4, TXT_BODY, desc_col, tb_meta[i].desc);
        if (!tb_meta[i].enabled) {
            const char* tag = "soon";
            add_label(scr, x + rw - text_w(tag, TXT_SMALL) - 16, y + (TB_ROW_H - TXT_SMALL) / 2, TXT_SMALL, COL_AMBER,
                      tag);
        }
        y += TB_ROW_H + 14;
    }

    int fy = h - TB_FOOTER_H;
    add_rect(scr, 0, fy, w, TB_FOOTER_H, COL_HEADER);
    add_rect(scr, 0, fy, w, 1, COL_PAGER_ACCENT);
    const char* hint = "WS: nav   Enter: open   ";
    int         ty   = fy + (TB_FOOTER_H - TXT_SMALL) / 2;
    add_label(scr, 10, ty, TXT_SMALL, COL_HINT, hint);
    add_back_hint(scr, 10 + text_w(hint, TXT_SMALL), ty, ": settings", TXT_SMALL);
}
