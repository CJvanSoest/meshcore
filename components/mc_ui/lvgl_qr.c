// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// the QR overlay.

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

// ── QR overlay (contact / channel) ───────────────────────────────────────────
// Full-screen overlay drawn on top of the Nodes/Channel base view. Pixel-matched
// port of render_qr_overlay() in render_nodes.c. The qrcodegen module produces a
// module bitmap; we wrap it at module resolution into an lv_image_dsc_t and
// upscale by an integer factor (nearest-neighbour, antialias off) so the cells
// stay crisp without materialising one lv_obj per module.

#define QR_MAX_SIZE 77  // qrcodegen version 15 → 17 + 4*15

static uint32_t       s_qr_argb[QR_MAX_SIZE * QR_MAX_SIZE];
static lv_image_dsc_t s_qr_dsc;

// Centred label helper for the QR overlay captions.
static void add_label_centered(lv_obj_t* scr, int w, int y, int size, uint32_t col, const char* text) {
    add_label(scr, (w - text_w(text, size)) / 2, y, size, col, text);
}

void render_qr_overlay_lvgl(void) {
    int       w   = (int)lvgl_port_width();
    int       h   = (int)lvgl_port_height();
    lv_obj_t* scr = begin_screen(COL_BG);
    pt_reset();

    char        url[256];
    const char* title_label  = NULL;
    char        subtitle[96] = {0};

    if (qr_overlay_mode == QR_MODE_CHANNEL) {
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
    enum qrcodegen_Ecc ecc = qrcodegen_Ecc_MEDIUM;
    bool ok = qrcodegen_encodeText(url, tmp_buf, qr_data, ecc, qrcodegen_VERSION_MIN, 15, qrcodegen_Mask_AUTO, true);

    if (!ok) {
        add_label(scr, 20, h / 2, TXT_BODY, COL_AMBER, "QR encode failed");
        return;
    }

    int qr_size = qrcodegen_getSize(qr_data);
    if (qr_size > QR_MAX_SIZE) {
        add_label(scr, 20, h / 2, TXT_BODY, COL_AMBER, "QR too large");
        return;
    }
    int max_px  = (h * 6) / 10;
    int cell_px = max_px / qr_size;
    if (cell_px < 2) cell_px = 2;
    int qr_px = cell_px * qr_size;
    int qr_x  = (w - qr_px) / 2;
    int qr_y  = (h - qr_px) / 2;

    int margin = cell_px * 2;
    // White quiet-zone behind the code; the code image carries its own white
    // background for the 0-modules so the two read as one clean block.
    add_rect(scr, qr_x - margin, qr_y - margin, qr_px + margin * 2, qr_px + margin * 2, 0xFFFFFFFF);

    for (int row = 0; row < qr_size; row++) {
        for (int col = 0; col < qr_size; col++) {
            s_qr_argb[row * qr_size + col] = qrcodegen_getModule(qr_data, col, row) ? 0xFF000000u : 0xFFFFFFFFu;
        }
    }
    s_qr_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    s_qr_dsc.header.cf     = LV_COLOR_FORMAT_ARGB8888;
    s_qr_dsc.header.flags  = 0;
    s_qr_dsc.header.w      = qr_size;
    s_qr_dsc.header.h      = qr_size;
    s_qr_dsc.header.stride = qr_size * 4;
    s_qr_dsc.data          = (const uint8_t*)s_qr_argb;
    s_qr_dsc.data_size     = (uint32_t)(qr_size * qr_size * 4);

    lv_obj_t* im = lv_image_create(scr);
    lv_image_set_src(im, &s_qr_dsc);
    lv_image_set_antialias(im, false);
    lv_image_set_pivot(im, 0, 0);
    lv_image_set_scale(im, 256 * cell_px);
    lv_obj_set_pos(im, qr_x, qr_y);

    add_label_centered(scr, w, qr_y - margin - TXT_TITLE - 6, TXT_TITLE, COL_AMBER, title_label);
    add_label_centered(scr, w, qr_y + qr_px + margin + 6, TXT_SMALL, COL_GRAY, subtitle);

    int next_y = qr_y + qr_px + margin + 6 + TXT_SMALL + 6;
    if (qr_overlay_mode == QR_MODE_CHANNEL) {
        const channel_t* ch =
            (qr_channel_idx >= 0 && qr_channel_idx < channel_count) ? &channels[qr_channel_idx] : NULL;
        if (ch) {
            char secret_hex[2 * CHANNEL_SECRET_LEN + 1] = {0};
            for (int i = 0; i < CHANNEL_SECRET_LEN; i++) snprintf(&secret_hex[i * 2], 3, "%02x", ch->secret[i]);
            add_label_centered(scr, w, next_y, TXT_SMALL, COL_GRAY, secret_hex);
            next_y += TXT_SMALL + 6;
        }
    }

    add_label_centered(scr, w, next_y, TXT_SMALL, COL_AMBER, "Press the red X to close");
}
