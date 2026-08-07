// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// VIEW_NODES.

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

// ── VIEW_NODES ───────────────────────────────────────────────────────────────
// Scrolling contacts + nodes list with row selection. The QR overlay reachable
// from here (Q) is drawn by lvgl_qr.c.

#define NODES_ROW_H    44
#define NODES_Y0       (TAB_BAR_H + 4)
#define NODES_HEADER_H 26

void render_nodes_lvgl(void) {
    int       w   = (int)lvgl_port_width();
    int       h   = (int)lvgl_port_height();
    lv_obj_t* scr = begin_screen(COL_BG);
    pt_reset();

    tab_bar_lvgl(scr);

    int hdr_y = NODES_Y0;
    add_rect(scr, 0, hdr_y, w, NODES_HEADER_H, COL_HEADER);
    add_rect(scr, 0, hdr_y + NODES_HEADER_H - 1, w, 1, COL_PANEL);

    int age_col_w  = 60;
    int dist_col_w = 60;
    int pkts_col_w = 60;
    int snr_col_w  = 54;
    int rssi_col_w = 64;
    int age_hdr_x  = w - age_col_w - 6;
    int dist_hdr_x = age_hdr_x - dist_col_w;
    int pkts_hdr_x = dist_hdr_x - pkts_col_w;
    int snr_hdr_x  = pkts_hdr_x - snr_col_w;
    int rssi_hdr_x = snr_hdr_x - rssi_col_w;
    int hdr_text_y = hdr_y + (NODES_HEADER_H - TXT_SMALL) / 2;
    add_label(scr, 8, hdr_text_y, TXT_SMALL, COL_WHITE, "Role");
    add_label(scr, 96, hdr_text_y, TXT_SMALL, COL_WHITE, "Name");
    add_label(scr, rssi_hdr_x, hdr_text_y, TXT_SMALL, COL_WHITE, "RSSI");
    add_label(scr, snr_hdr_x, hdr_text_y, TXT_SMALL, COL_WHITE, "SNR");
    add_label(scr, pkts_hdr_x, hdr_text_y, TXT_SMALL, COL_WHITE, "#Pkt");
    add_label(scr, dist_hdr_x, hdr_text_y, TXT_SMALL, COL_WHITE, "Dist");
    add_label(scr, age_hdr_x, hdr_text_y, TXT_SMALL, COL_WHITE, "Seen");

    int      list_y0    = NODES_Y0 + NODES_HEADER_H;
    int      footer_h   = 60;
    int      list_h     = h - footer_h - list_y0;
    int      rows_vis   = list_h / NODES_ROW_H;
    uint32_t now_ms     = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    int      row_text_y = (NODES_ROW_H - TXT_BODY) / 2;

    if (!lora_rx_ok) {
        add_label(scr, 12, list_y0 + 14, TXT_BODY, COL_AMBER, "LoRa radio not available");
        add_label(scr, 12, list_y0 + 14 + TXT_BODY + 8, TXT_BODY, COL_GRAY,
                  "Update via Launcher: Tools > Firmware update");
    } else if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (node_count == 0 && contact_count == 0) {
            add_label(scr, 12, list_y0 + 14, TXT_BODY, COL_GRAY, "Listening... no nodes heard yet.");
        } else {
            display_row_t* rows_dl   = node_display_rows();
            int            idx_count = build_node_display(rows_dl, NODE_DISPLAY_ROWS_MAX);

            int max_scroll = idx_count - rows_vis;
            if (max_scroll < 0) max_scroll = 0;
            if (node_scroll > max_scroll) node_scroll = max_scroll;
            if (node_scroll < 0) node_scroll = 0;

            if (node_cursor >= idx_count) node_cursor = idx_count > 0 ? idx_count - 1 : 0;
            if (node_cursor < 0) node_cursor = 0;
            if (node_cursor < node_scroll) node_scroll = node_cursor;
            if (node_cursor >= node_scroll + rows_vis) node_scroll = node_cursor - rows_vis + 1;

            bool divider_drawn = false;
            for (int row = 0; row < rows_vis; row++) {
                int list_idx = row + node_scroll;
                if (list_idx >= idx_count) break;
                display_row_t* d = &rows_dl[list_idx];
                node_entry_t*  n = (d->node_idx >= 0) ? &node_list[d->node_idx] : NULL;

                int  y         = list_y0 + row * NODES_ROW_H;
                bool is_cursor = (list_idx == node_cursor);

                if (!divider_drawn && !d->is_contact) {
                    bool prev_was_contact       = (row > 0 && rows_dl[list_idx - 1].is_contact);
                    bool scrolled_past_contacts = (node_scroll > 0 && contact_count > 0 && list_idx <= contact_count);
                    if (prev_was_contact || scrolled_past_contacts) {
                        add_rect(scr, 6, y - 2, w - 12, 2, COL_AMBER);
                    }
                    divider_drawn = true;
                }

                if (is_cursor) {
                    add_rect(scr, 0, y, w, NODES_ROW_H, COL_PANEL);
                    add_rect(scr, 0, y, 5, NODES_ROW_H, COL_ACCENT);
                }

                int age_x  = w - age_col_w - 6;
                int dist_x = age_x - dist_col_w;
                int pkts_x = dist_x - pkts_col_w;
                int snr_x  = pkts_x - snr_col_w;
                int rssi_x = snr_x - rssi_col_w;

                char age_buf[20];
                if (n) {
                    uint32_t age_s = (now_ms - n->last_seen_ms) / 1000;
                    if (age_s < 60)
                        snprintf(age_buf, sizeof(age_buf), "%lus", (unsigned long)age_s);
                    else if (age_s < 3600)
                        snprintf(age_buf, sizeof(age_buf), "%lum", (unsigned long)(age_s / 60));
                    else
                        snprintf(age_buf, sizeof(age_buf), "%luh", (unsigned long)(age_s / 3600));
                } else {
                    snprintf(age_buf, sizeof(age_buf), "--");
                }
                add_label(scr, age_x, y + row_text_y, TXT_BODY, COL_GRAY, age_buf);

                char dist_buf[12];
                if (n && n->position_valid && gps_position_valid) {
                    const double k    = 1e-6 * (M_PI / 180.0);
                    double       lat1 = (double)gps_lat_e6 * k;
                    double       lon1 = (double)gps_lon_e6 * k;
                    double       lat2 = (double)n->lat * k;
                    double       lon2 = (double)n->lon * k;
                    double       xx   = (lon2 - lon1) * cos((lat1 + lat2) * 0.5);
                    double       yy   = (lat2 - lat1);
                    double       d_km = 6371.0 * sqrt(xx * xx + yy * yy);
                    if (d_km < 1.0)
                        snprintf(dist_buf, sizeof(dist_buf), "%dm", (int)(d_km * 1000.0));
                    else if (d_km < 10.0)
                        snprintf(dist_buf, sizeof(dist_buf), "%.1fkm", d_km);
                    else
                        snprintf(dist_buf, sizeof(dist_buf), "%dkm", (int)d_km);
                } else {
                    snprintf(dist_buf, sizeof(dist_buf), "--");
                }
                add_label(scr, dist_x, y + row_text_y, TXT_BODY, COL_GRAY, dist_buf);

                char pkts_buf[8];
                if (n)
                    snprintf(pkts_buf, sizeof(pkts_buf), "#%d", n->packet_count);
                else
                    snprintf(pkts_buf, sizeof(pkts_buf), "--");
                add_label(scr, pkts_x, y + row_text_y, TXT_BODY, COL_GRAY, pkts_buf);

                char     rssi_buf[8], snr_buf[8];
                uint32_t rssi_col, snr_col;
                if (n && n->stats_valid) {
                    int rssi_dbm = (int)n->last_rssi_dbm;
                    int snr_dB   = (int)n->last_snr_db_x4 / 4;
                    snprintf(rssi_buf, sizeof(rssi_buf), "%d", rssi_dbm);
                    snprintf(snr_buf, sizeof(snr_buf), "%+d", snr_dB);
                    rssi_col = (rssi_dbm >= -80) ? COL_GREEN : (rssi_dbm >= -105) ? COL_AMBER : COL_RED;
                    snr_col  = (snr_dB >= 0) ? COL_GREEN : (snr_dB >= -10) ? COL_AMBER : COL_RED;
                } else {
                    snprintf(rssi_buf, sizeof(rssi_buf), "--");
                    snprintf(snr_buf, sizeof(snr_buf), "--");
                    rssi_col = COL_GRAY;
                    snr_col  = COL_GRAY;
                }
                add_label(scr, rssi_x, y + row_text_y, TXT_BODY, rssi_col, rssi_buf);
                add_label(scr, snr_x, y + row_text_y, TXT_BODY, snr_col, snr_buf);

                meshcore_device_role_t role     = n ? n->role : (meshcore_device_role_t)contacts[d->contact_idx].role;
                const char*            src_name = n ? n->name : contacts[d->contact_idx].alias;

                const char* rl       = role_label(role);
                uint32_t    role_col = (role == MESHCORE_DEVICE_ROLE_REPEATER)      ? COL_BLUE
                                       : (role == MESHCORE_DEVICE_ROLE_ROOM_SERVER) ? 0xFFBB9AF7
                                       : (role == MESHCORE_DEVICE_ROLE_SENSOR)      ? COL_AMBER
                                                                                    : COL_GREEN;
                add_label(scr, 8, y + row_text_y, TXT_BODY, role_col, rl);

                int name_x = 96;
                if (d->is_contact) {
                    add_label(scr, 78, y + row_text_y, TXT_BODY, COL_AMBER, "*");
                }

                int row_unread   = d->is_contact ? contact_unread[d->contact_idx] : 0;
                int badge_w_resv = 0;
                if (row_unread > 0) {
                    char ub[8];
                    snprintf(ub, sizeof(ub), "%d", row_unread > 99 ? 99 : row_unread);
                    badge_w_resv = text_w(ub, TXT_SMALL) + 12 + 6;
                }

                char name_trunc[25];
                int  max_name_w = rssi_x - name_x - 6 - badge_w_resv;
                int  max_chars  = max_name_w / 11;
                if (max_chars > 24) max_chars = 24;
                if (max_chars < 1) max_chars = 1;
                snprintf(name_trunc, sizeof(name_trunc), "%.*s", max_chars, src_name);
                uint32_t name_col = is_cursor ? COL_WHITE : (n == NULL ? COL_GRAY : COL_WHITE);
                add_label(scr, name_x, y + row_text_y, TXT_BODY, name_col, name_trunc);

                if (row_unread > 0) {
                    char ub[8];
                    snprintf(ub, sizeof(ub), "%d", row_unread > 99 ? 99 : row_unread);
                    int nw = text_w(name_trunc, TXT_BODY);
                    int bw = text_w(ub, TXT_SMALL) + 12;
                    int bx = name_x + nw + 6;
                    int by = y + (NODES_ROW_H - (TXT_SMALL + 4)) / 2;
                    add_rect(scr, bx, by, bw, TXT_SMALL + 4, COL_RED);
                    add_label(scr, bx + 6, by + 2, TXT_SMALL, COL_HEADER, ub);
                }

                add_rect(scr, 12, y + NODES_ROW_H - 1, w - 24, 1, COL_PANEL);
            }

            if (idx_count > rows_vis) {
                char sc[24];
                snprintf(sc, sizeof(sc), "%d/%d", node_scroll + 1, idx_count);
                add_label(scr, w - text_w(sc, TXT_SMALL) - 10, h - footer_h - TXT_SMALL - 2, TXT_SMALL, COL_GRAY, sc);
            }
            if (idx_count == 0 && (node_count > 0 || contact_count > 0)) {
                add_label(scr, 12, list_y0 + 14, TXT_BODY, COL_AMBER,
                          "No entries match the active filter -- press L to clear");
            }
        }
        xSemaphoreGive(node_mutex);
    }

    int fy_base = h - footer_h;
    add_rect(scr, 0, fy_base, w, footer_h, COL_HEADER);
    add_rect(scr, 0, fy_base, w, 1, COL_PANEL);

    int  fx      = 10;
    int  fy_text = fy_base + 6;
    char counts[48];
    snprintf(counts, sizeof(counts), "Nodes:%d/%d  Contacts:%d/%d", node_count, MAX_NODES, contact_count, MAX_CONTACTS);
    add_label(scr, fx, fy_text, TXT_BODY, COL_WHITE, counts);
    fx += text_w(counts, TXT_BODY) + 20;

    if (node_filter != MESHCORE_DEVICE_ROLE_UNKNOWN) {
        char pill[40];
        snprintf(pill, sizeof(pill), "filter: %s", role_label(node_filter));
        int pw = text_w(pill, TXT_BODY);
        add_rect(scr, fx - 6, fy_text - 2, pw + 12, TXT_BODY + 4, COL_AMBER);
        add_label(scr, fx, fy_text, TXT_BODY, COL_HEADER, pill);
        fx += pw + 22;
    }

    const char* ctrl   = (node_filter == MESHCORE_DEVICE_ROLE_UNKNOWN) ? "W/S nav   A:advert   F:fav   L:filter   Q:QR"
                                                                       : "L:next   F:fav   A:advert";
    int         ctrl_y = fy_text + (TXT_BODY - TXT_SMALL) / 2;
    add_label(scr, fx, ctrl_y, TXT_SMALL, COL_HINT, ctrl);
    add_back_hint(scr, fx + text_w(ctrl, TXT_SMALL) + 16, ctrl_y, ": home", TXT_SMALL);

    if (identity_is_ready()) {
        uint32_t now_ms2 = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        char     adv_buf[48];
        if (last_advert_ms == 0) {
            snprintf(adv_buf, sizeof(adv_buf), "advert: pending");
        } else {
            uint32_t age_s = (now_ms2 - last_advert_ms) / 1000;
            snprintf(adv_buf, sizeof(adv_buf), "last advert: %lus ago", (unsigned long)age_s);
        }
        add_label(scr, 10, fy_text + TXT_BODY + 6, TXT_SMALL, COL_GRAY, adv_buf);
    }
}
