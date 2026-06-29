// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "channels.h"
#include "contacts.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "identity.h"
#include "nodes.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"
#include "qrcodegen.h"
#include "radio.h"
#include "render.h"
#include "render_internal.h"
#include "settings_nvs.h"
#include "ui_state.h"
#include "wifi_connection.h"

#define NODES_ROW_H    44
#define NODES_Y0       (TAB_BAR_H + 4)
#define NODES_HEADER_H 26

void render_nodes(void) {
    int w = (int)pax_buf_get_width(&fb);
    int h = (int)pax_buf_get_height(&fb);

    pax_background(&fb, COL_BG);
    render_tab_bar();

    int hdr_y = NODES_Y0;
    pax_simple_rect(&fb, COL_HEADER, 0, hdr_y, w, NODES_HEADER_H);
    pax_simple_rect(&fb, COL_PANEL, 0, hdr_y + NODES_HEADER_H - 1, w, 1);

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
    pax_draw_text(&fb, COL_WHITE, FONT, TXT_SMALL, 8, hdr_text_y, "Role");
    pax_draw_text(&fb, COL_WHITE, FONT, TXT_SMALL, 96, hdr_text_y, "Name");
    pax_draw_text(&fb, COL_WHITE, FONT, TXT_SMALL, rssi_hdr_x, hdr_text_y, "RSSI");
    pax_draw_text(&fb, COL_WHITE, FONT, TXT_SMALL, snr_hdr_x, hdr_text_y, "SNR");
    pax_draw_text(&fb, COL_WHITE, FONT, TXT_SMALL, pkts_hdr_x, hdr_text_y, "#Pkt");
    pax_draw_text(&fb, COL_WHITE, FONT, TXT_SMALL, dist_hdr_x, hdr_text_y, "Dist");
    pax_draw_text(&fb, COL_WHITE, FONT, TXT_SMALL, age_hdr_x, hdr_text_y, "Seen");

    int      list_y0    = NODES_Y0 + NODES_HEADER_H;
    int      footer_h   = 60;
    int      list_h     = h - footer_h - list_y0;
    int      rows_vis   = list_h / NODES_ROW_H;
    uint32_t now_ms     = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    int      row_text_y = (NODES_ROW_H - TXT_BODY) / 2;

    if (!lora_rx_ok) {
        pax_draw_text(&fb, COL_AMBER, FONT, TXT_BODY, 12, list_y0 + 14, "LoRa radio not available");
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_BODY, 12, list_y0 + 14 + TXT_BODY + 8,
                      "Update via Launcher: Tools > Firmware update");
    } else if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (node_count == 0 && contact_count == 0) {
            pax_draw_text(&fb, COL_GRAY, FONT, TXT_BODY, 12, list_y0 + 14, "Listening... no nodes heard yet.");
        } else {
            display_row_t rows_dl[MAX_CONTACTS + MAX_NODES];
            int           idx_count = build_node_display(rows_dl, MAX_CONTACTS + MAX_NODES);

            int max_scroll = idx_count - rows_vis;
            if (max_scroll < 0) max_scroll = 0;
            if (node_scroll > max_scroll) node_scroll = max_scroll;
            if (node_scroll < 0) node_scroll = 0;

            if (node_cursor >= idx_count) node_cursor = idx_count > 0 ? idx_count - 1 : 0;
            if (node_cursor < 0) node_cursor = 0;
            if (node_cursor < node_scroll) node_scroll = node_cursor;
            if (node_cursor >= node_scroll + rows_vis) node_scroll = node_cursor - rows_vis + 1;

            // Orange divider between the contact (favorite) section at the
            // top and the discovered-nodes section below. We draw it at the
            // top edge of the first non-contact row, but only if at least
            // one contact row precedes it in the visible window.
            bool divider_drawn = false;
            for (int row = 0; row < rows_vis; row++) {
                int list_idx = row + node_scroll;
                if (list_idx >= idx_count) break;
                display_row_t* d = &rows_dl[list_idx];
                node_entry_t*  n = (d->node_idx >= 0) ? &node_list[d->node_idx] : NULL;

                int  y         = list_y0 + row * NODES_ROW_H;
                bool is_cursor = (list_idx == node_cursor);

                if (!divider_drawn && !d->is_contact) {
                    // First non-contact row in the visible window. Only
                    // draw if the previous visible row was a contact, OR
                    // this row is not the very top (i.e. contacts were
                    // scrolled off but did exist).
                    bool prev_was_contact       = (row > 0 && rows_dl[list_idx - 1].is_contact);
                    bool scrolled_past_contacts = (node_scroll > 0 && contact_count > 0 && list_idx <= contact_count);
                    if (prev_was_contact || scrolled_past_contacts) {
                        pax_simple_rect(&fb, COL_AMBER, 6, y - 2, w - 12, 2);
                    }
                    divider_drawn = true;
                }

                if (is_cursor) {
                    pax_simple_rect(&fb, COL_PANEL, 0, y, w, NODES_ROW_H);
                    pax_simple_rect(&fb, COL_ACCENT, 0, y, 5, NODES_ROW_H);
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
                pax_draw_text(&fb, COL_GRAY, FONT, TXT_BODY, age_x, y + row_text_y, age_buf);

                char dist_buf[12];
                if (n && n->position_valid && gps_position_valid) {
                    const double k    = 1e-6 * (M_PI / 180.0);
                    double       lat1 = (double)gps_lat_e6 * k;
                    double       lon1 = (double)gps_lon_e6 * k;
                    double       lat2 = (double)n->lat * k;
                    double       lon2 = (double)n->lon * k;
                    double       x    = (lon2 - lon1) * cos((lat1 + lat2) * 0.5);
                    double       y_   = (lat2 - lat1);
                    double       d_km = 6371.0 * sqrt(x * x + y_ * y_);
                    if (d_km < 1.0)
                        snprintf(dist_buf, sizeof(dist_buf), "%dm", (int)(d_km * 1000.0));
                    else if (d_km < 10.0)
                        snprintf(dist_buf, sizeof(dist_buf), "%.1fkm", d_km);
                    else
                        snprintf(dist_buf, sizeof(dist_buf), "%dkm", (int)d_km);
                } else {
                    snprintf(dist_buf, sizeof(dist_buf), "--");
                }
                pax_draw_text(&fb, COL_GRAY, FONT, TXT_BODY, dist_x, y + row_text_y, dist_buf);

                char pkts_buf[8];
                if (n)
                    snprintf(pkts_buf, sizeof(pkts_buf), "#%d", n->packet_count);
                else
                    snprintf(pkts_buf, sizeof(pkts_buf), "--");
                pax_draw_text(&fb, COL_GRAY, FONT, TXT_BODY, pkts_x, y + row_text_y, pkts_buf);

                char      rssi_buf[8], snr_buf[8];
                pax_col_t rssi_col, snr_col;
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
                pax_draw_text(&fb, rssi_col, FONT, TXT_BODY, rssi_x, y + row_text_y, rssi_buf);
                pax_draw_text(&fb, snr_col, FONT, TXT_BODY, snr_x, y + row_text_y, snr_buf);

                meshcore_device_role_t role     = n ? n->role : (meshcore_device_role_t)contacts[d->contact_idx].role;
                const char*            src_name = n ? n->name : contacts[d->contact_idx].alias;

                const char* rl       = role_label(role);
                pax_col_t   role_col = (role == MESHCORE_DEVICE_ROLE_REPEATER)      ? COL_BLUE
                                       : (role == MESHCORE_DEVICE_ROLE_ROOM_SERVER) ? 0xFFBB9AF7
                                       : (role == MESHCORE_DEVICE_ROLE_SENSOR)      ? COL_AMBER
                                                                                    : COL_GREEN;
                pax_draw_text(&fb, role_col, FONT, TXT_BODY, 8, y + row_text_y, rl);

                int name_x = 96;
                if (d->is_contact) {
                    pax_draw_text(&fb, COL_AMBER, FONT, TXT_BODY, 78, y + row_text_y, "*");
                }

                int row_unread   = d->is_contact ? contact_unread[d->contact_idx] : 0;
                int badge_w_resv = 0;
                if (row_unread > 0) {
                    char ub[8];
                    snprintf(ub, sizeof(ub), "%d", row_unread > 99 ? 99 : row_unread);
                    badge_w_resv = (int)pax_text_size(FONT, TXT_SMALL, ub).x + 12 + 6;
                }

                char name_trunc[25];
                int  max_name_w = rssi_x - name_x - 6 - badge_w_resv;
                int  max_chars  = max_name_w / 11;
                if (max_chars > 24) max_chars = 24;
                if (max_chars < 1) max_chars = 1;
                snprintf(name_trunc, sizeof(name_trunc), "%.*s", max_chars, src_name);
                pax_col_t name_col = is_cursor ? COL_WHITE : (n == NULL ? COL_GRAY : COL_WHITE);
                pax_draw_text(&fb, name_col, FONT, TXT_BODY, name_x, y + row_text_y, name_trunc);

                if (row_unread > 0) {
                    char ub[8];
                    snprintf(ub, sizeof(ub), "%d", row_unread > 99 ? 99 : row_unread);
                    pax_vec2f nsz = pax_text_size(FONT, TXT_BODY, name_trunc);
                    pax_vec2f usz = pax_text_size(FONT, TXT_SMALL, ub);
                    int       bw  = (int)usz.x + 12;
                    int       bx  = name_x + (int)nsz.x + 6;
                    int       by  = y + (NODES_ROW_H - (TXT_SMALL + 4)) / 2;
                    pax_simple_rect(&fb, COL_RED, bx, by, bw, TXT_SMALL + 4);
                    pax_draw_text(&fb, COL_HEADER, FONT, TXT_SMALL, bx + 6, by + 2, ub);
                }

                pax_simple_rect(&fb, COL_PANEL, 12, y + NODES_ROW_H - 1, w - 24, 1);
            }

            if (idx_count > rows_vis) {
                char sc[24];
                snprintf(sc, sizeof(sc), "%d/%d", node_scroll + 1, idx_count);
                pax_vec2f sz = pax_text_size(FONT, TXT_SMALL, sc);
                pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, w - (int)sz.x - 10, h - footer_h - TXT_SMALL - 2, sc);
            }
            if (idx_count == 0 && (node_count > 0 || contact_count > 0)) {
                pax_draw_text(&fb, COL_AMBER, FONT, TXT_BODY, 12, list_y0 + 14,
                              "No entries match the active filter — press L to clear");
            }
        }
        xSemaphoreGive(node_mutex);
    }

    int fy_base = h - footer_h;
    pax_simple_rect(&fb, COL_HEADER, 0, fy_base, w, footer_h);
    pax_simple_rect(&fb, COL_PANEL, 0, fy_base, w, 1);

    int  fx      = 10;
    int  fy_text = fy_base + 6;
    char counts[48];
    snprintf(counts, sizeof(counts), "Nodes:%d  Contacts:%d", node_count, contact_count);
    pax_vec2f csz = pax_text_size(FONT, TXT_BODY, counts);
    pax_draw_text(&fb, COL_WHITE, FONT, TXT_BODY, fx, fy_text, counts);
    fx += (int)csz.x + 20;

    if (node_filter != MESHCORE_DEVICE_ROLE_UNKNOWN) {
        char pill[40];
        snprintf(pill, sizeof(pill), "filter: %s", role_label(node_filter));
        pax_vec2f psz = pax_text_size(FONT, TXT_BODY, pill);
        pax_simple_rect(&fb, COL_AMBER, fx - 6, fy_text - 2, (int)psz.x + 12, TXT_BODY + 4);
        pax_draw_text(&fb, COL_HEADER, FONT, TXT_BODY, fx, fy_text, pill);
        fx += (int)psz.x + 22;
    }

    const char* ctrl   = (node_filter == MESHCORE_DEVICE_ROLE_UNKNOWN) ? "W/S nav   A:advert   F:fav   L:filter   Q:QR"
                                                                       : "L:next   F:fav   A:advert";
    int         ctrl_y = fy_text + (TXT_BODY - TXT_SMALL) / 2;
    pax_draw_text(&fb, COL_HINT, FONT, TXT_SMALL, fx, ctrl_y, ctrl);
    render_back_hint(fx + (int)pax_text_size(FONT, TXT_SMALL, ctrl).x + 16, ctrl_y, ": home", TXT_SMALL);

    if (identity_is_ready()) {
        uint32_t now_ms2 = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        char     adv_buf[48];
        if (last_advert_ms == 0) {
            snprintf(adv_buf, sizeof(adv_buf), "advert: pending");
        } else {
            uint32_t age_s = (now_ms2 - last_advert_ms) / 1000;
            snprintf(adv_buf, sizeof(adv_buf), "last advert: %lus ago", (unsigned long)age_s);
        }
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 10, fy_text + TXT_BODY + 6, adv_buf);
    }
}

void render_qr_overlay(void) {
    int w = (int)pax_buf_get_width(&fb);
    int h = (int)pax_buf_get_height(&fb);

    char        url[256];
    const char* title_label  = NULL;
    char        subtitle[96] = {0};

    if (qr_overlay_mode == QR_MODE_OWNTRACKS) {
        // The HTTPS URL keys off the live IP when WiFi is up, falling back to the
        // mDNS name so the QR still encodes something resolvable even before the
        // first DHCP lease. The full API key is embedded so the iPhone Camera app
        // captures the entire URL — the user can paste it directly into OwnTracks
        // / Shortcuts / MeshMapper without retyping 64 hex chars.
        const char* host         = "tanmatsu.local";
        char        host_buf[24] = {0};
        if (wifi_connection_is_connected()) {
            esp_netif_ip_info_t* ip = wifi_get_ip_info();
            if (ip && ip->ip.addr) {
                snprintf(host_buf, sizeof(host_buf), IPSTR, IP2STR(&ip->ip));
                host = host_buf;
            }
        }
        snprintf(url, sizeof(url), "https://%s:8443/ping?key=%s", host, http_api_key[0] ? http_api_key : "(unset)");
        title_label = "Scan for OwnTracks";
        snprintf(subtitle, sizeof(subtitle), "https://%s:8443/ping", host);
    } else if (qr_overlay_mode == QR_MODE_CHANNEL) {
        // Channel share link: name is percent-encoded (the leading '#' becomes
        // %23, which channel_parse_share decodes back); secret is 32 lowercase
        // hex — the exact upstream meshcore://channel/add format.
        const channel_t* ch =
            (qr_channel_idx >= 0 && qr_channel_idx < channel_count) ? &channels[qr_channel_idx] : NULL;
        char hex_key[2 * CHANNEL_SECRET_LEN + 1] = {0};
        char enc_name[72]                        = {0};
        int  ei                                  = 0;
        if (ch) {
            for (int i = 0; i < CHANNEL_SECRET_LEN; i++) snprintf(&hex_key[i * 2], 3, "%02x", ch->secret[i]);
            static const char hexd[] = "0123456789abcdef";
            for (int i = 0; ch->name[i] && ei < (int)sizeof(enc_name) - 4; i++) {
                unsigned char c = (unsigned char)ch->name[i];
                if (c == ' ') {
                    enc_name[ei++] = '+';
                } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                           c == '_' || c == '.') {
                    enc_name[ei++] = (char)c;
                } else {
                    enc_name[ei++] = '%';
                    enc_name[ei++] = hexd[c >> 4];
                    enc_name[ei++] = hexd[c & 0xf];
                }
            }
        }
        snprintf(url, sizeof(url), "meshcore://channel/add?name=%s&secret=%s", enc_name, hex_key);
        title_label = "Scan to add channel";
        snprintf(subtitle, sizeof(subtitle), "%s", ch ? ch->name : "(no channel)");
    } else {
        char hex_key[65];
        for (int i = 0; i < 32; i++) snprintf(&hex_key[i * 2], 3, "%02x", node_pub_key[i]);
        hex_key[64] = '\0';

        const char* adv_src =
            lora_advert_name[0] ? lora_advert_name : ((owner_name[0] && owner_name[0] != '(') ? owner_name : "");

        char encoded_name[64];
        int  ei = 0;
        for (int i = 0; adv_src[i] && ei < 62; i++) {
            char c = adv_src[i];
            if (c == ' ') {
                encoded_name[ei++] = '+';
            } else {
                encoded_name[ei++] = c;
            }
        }
        encoded_name[ei] = '\0';

        snprintf(url, sizeof(url), "meshcore://contact/add?name=%s&public_key=%s&type=1", encoded_name, hex_key);
        title_label = "Scan to add contact";
        snprintf(subtitle, sizeof(subtitle), "%s", adv_src[0] ? adv_src : "(no name)");
    }

    static uint8_t     qr_data[qrcodegen_BUFFER_LEN_MAX];
    static uint8_t     tmp_buf[qrcodegen_BUFFER_LEN_MAX];
    // OwnTracks URL with embedded 64-char key is ~110 chars → needs higher
    // version cap than the original contact QR. Drop ECC to LOW only for that
    // mode so the cell-density stays scan-friendly at 480 px tall.
    enum qrcodegen_Ecc ecc = (qr_overlay_mode == QR_MODE_OWNTRACKS) ? qrcodegen_Ecc_LOW : qrcodegen_Ecc_MEDIUM;
    bool ok = qrcodegen_encodeText(url, tmp_buf, qr_data, ecc, qrcodegen_VERSION_MIN, 15, qrcodegen_Mask_AUTO, true);

    pax_background(&fb, COL_BG);

    if (!ok) {
        pax_draw_text(&fb, COL_AMBER, FONT, TXT_BODY, 20, h / 2, "QR encode failed");
        return;
    }

    int qr_size = qrcodegen_getSize(qr_data);
    int max_px  = (h * 6) / 10;
    int cell_px = max_px / qr_size;
    if (cell_px < 2) cell_px = 2;
    int qr_px = cell_px * qr_size;
    int qr_x  = (w - qr_px) / 2;
    int qr_y  = (h - qr_px) / 2;

    int margin = cell_px * 2;
    pax_simple_rect(&fb, 0xFFFFFFFF, qr_x - margin, qr_y - margin, qr_px + margin * 2, qr_px + margin * 2);

    for (int row = 0; row < qr_size; row++) {
        for (int col = 0; col < qr_size; col++) {
            if (qrcodegen_getModule(qr_data, col, row)) {
                pax_simple_rect(&fb, 0xFF000000, qr_x + col * cell_px, qr_y + row * cell_px, cell_px, cell_px);
            }
        }
    }

    pax_vec2f lsz = pax_text_size(FONT, TXT_TITLE, title_label);
    pax_draw_text(&fb, COL_AMBER, FONT, TXT_TITLE, (w - (int)lsz.x) / 2, qr_y - margin - TXT_TITLE - 6, title_label);

    pax_vec2f nsz = pax_text_size(FONT, TXT_SMALL, subtitle);
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, (w - (int)nsz.x) / 2, qr_y + qr_px + margin + 6, subtitle);

    // OwnTracks mode adds a key-preview line so a user without a camera can
    // still verify the on-device key matches what their phone captured.
    int next_y = qr_y + qr_px + margin + 6 + TXT_SMALL + 6;
    if (qr_overlay_mode == QR_MODE_OWNTRACKS && http_api_key[0]) {
        char key_preview[40];
        snprintf(key_preview, sizeof(key_preview), "key %.8s...%.4s", http_api_key, http_api_key + 60);
        pax_vec2f ksz = pax_text_size(FONT, TXT_SMALL, key_preview);
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, (w - (int)ksz.x) / 2, next_y, key_preview);
        next_y += TXT_SMALL + 6;
    } else if (qr_overlay_mode == QR_MODE_CHANNEL) {
        // Print the 32-hex secret too, so a receiver without a camera can type
        // it (name + secret) into their device by hand.
        const channel_t* ch =
            (qr_channel_idx >= 0 && qr_channel_idx < channel_count) ? &channels[qr_channel_idx] : NULL;
        if (ch) {
            char secret_hex[2 * CHANNEL_SECRET_LEN + 1] = {0};
            for (int i = 0; i < CHANNEL_SECRET_LEN; i++) snprintf(&secret_hex[i * 2], 3, "%02x", ch->secret[i]);
            pax_vec2f ssz = pax_text_size(FONT, TXT_SMALL, secret_hex);
            pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, (w - (int)ssz.x) / 2, next_y, secret_hex);
            next_y += TXT_SMALL + 6;
        }
    }

    // Close hint: the red X on the Tanmatsu chassis dismisses the overlay (ESC
    // is no longer a back key in submenus).
    const char* close_hint = "Press the red X to close";
    pax_vec2f   csz        = pax_text_size(FONT, TXT_SMALL, close_hint);
    pax_draw_text(&fb, COL_AMBER, FONT, TXT_SMALL, (w - (int)csz.x) / 2, next_y, close_hint);
}
