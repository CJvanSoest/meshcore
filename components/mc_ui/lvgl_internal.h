// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

// Internal interface between the lvgl_*.c view files. Not for use outside
// mc_ui: the public entry point is lvgl_ui.h.

#pragma once

#include "chat.h"
#include "history.h"
#include "lvgl.h"

// List metrics shared by the Storage and BLE device lists
#define ST_HEADER_H 50
#define ST_ROW_H    30
#define ST_FOOTER_H 38

// Screen and widget primitives
lv_obj_t*  begin_screen(uint32_t bg_col);
lv_obj_t*  add_label(lv_obj_t* parent, int x, int y, int font_sz, uint32_t col, const char* text);
lv_obj_t*  add_rect(lv_obj_t* parent, int x, int y, int w, int h, uint32_t col);
void       add_line(lv_obj_t* p, int x1, int y1, int x2, int y2, int w, uint32_t col);
void       add_circle(lv_obj_t* p, int cx, int cy, int r, int64_t fill, int64_t border, int bw);
void       add_arc(lv_obj_t* p, int cx, int cy, int r, int start_deg, int end_deg, int w, uint32_t col);
void       add_cloud(lv_obj_t* p, int cx, int cy, int r, uint32_t col);
void       add_hex_cols(lv_obj_t* parent, int x, int y, uint32_t col, const uint8_t* bytes, int n);
void       add_back_hint(lv_obj_t* parent, int x, int y, const char* label, int ts);
int        text_w(const char* text, int font_sz);
lv_color_t mc_col(uint32_t argb);
void       pt_reset(void);

// Shared chrome
void tab_bar_lvgl(lv_obj_t* scr);
void status_toast_lvgl(lv_obj_t* scr, int w, int h);
void home_status_right(lv_obj_t* scr, int x_right, int ty, int font);

// Inline emoji
int  emoji_text(lv_obj_t* parent, int x, int y, int size, uint32_t col, const char* text);
void render_emoji_picker_overlay_lvgl(lv_obj_t* scr, int w, int h);

// Chat message ring
void render_msg_list_lvgl(lv_obj_t* scr, int w, int list_y0, int list_h, chat_msg_t* msgs, int head, int count,
                          int* scroll_p, bool is_channel);

// Settings category glyphs reused by the Home tiles
void cat_icon_wifi_lv(lv_obj_t* s, int cx, int cy, int sz, uint32_t col);
void cat_icon_bluetooth_lv(lv_obj_t* s, int cx, int cy, int sz, uint32_t col);
void cat_icon_toolbox_lv(lv_obj_t* s, int cx, int cy, int sz, uint32_t col);

// Per-view entry points, called from the dispatch in lvgl_ui.c
void render_about_lvgl(void);
void render_ble_devices_lvgl(void);
void render_channel_lvgl(void);
void render_chat_lvgl(void);
void render_home_lvgl(void);
void render_map_lvgl(void);
void render_nodes_lvgl(void);
void render_qr_overlay_lvgl(void);
void render_settings_lvgl(void);
void render_toolbox_coverage_lvgl(void);
void render_toolbox_log_lvgl(void);
void render_toolbox_lvgl(void);
void render_toolbox_storage_lvgl(void);
