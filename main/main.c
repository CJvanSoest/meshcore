/*
 * MeshCore for Tanmatsu
 *
 * A MeshCore LoRa mesh communication app for the Tanmatsu badge (ESP32-P4).
 * Supports DM (direct messages), public channel chat, node discovery,
 * LoRa radio configuration, and QR-code based contact sharing.
 *
 * SPDX-FileCopyrightText: 2026 CJ van Soest
 * SPDX-License-Identifier: MIT
 *
 * Developed with Claude AI (Anthropic) as AI co-author.
 *
 * Third-party components:
 *   qrcodegen.{c,h}  — © Project Nayuki, MIT License
 *                       https://www.nayuki.io/page/qr-code-generator-library
 *   ed25519.{c,h}    — Based on public domain NaCl/SUPERCOP ref10
 *                       (D.J. Bernstein et al.); ESP32 adaptation MIT License
 *   meshcore/        — © 2025 Scott Powell / rippleradios.com,
 *                       © 2025 Nicolai Electronics, MIT License
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "bsp/led.h"
#include "bsp/power.h"
#include "bsp/tanmatsu.h"
#include "tanmatsu_coprocessor.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"
#include "pax_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lora.h"
#include "wifi_connection.h"
#include "meshcore/packet.h"
#include "meshcore/payload/advert.h"
#include "meshcore/payload/grp_txt.h"
#include "mbedtls/aes.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include "esp_random.h"
#include "esp_sntp.h"
#include "ed25519.h"
#include "qrcodegen.h"
#if defined(CONFIG_IDF_TARGET_ESP32P4)
#include "esp_hosted.h"
#endif

static char const TAG[] = "main";

// ── Colors ────────────────────────────────────────────────────────────────────
#define COL_BLACK   0xFF000000
#define COL_WHITE   0xFFFFFFFF
#define COL_GRAY    0xFF888888
#define COL_DARK    0xFF222222
#define COL_ACCENT  0xFF0088FF
#define COL_GREEN   0xFF00BB44
#define COL_YELLOW  0xFFFFCC00
#define COL_RED     0xFFFF4444

// ── Display globals ───────────────────────────────────────────────────────────
static size_t                     display_h_res        = 0;
static size_t                     display_v_res        = 0;
static bsp_display_color_format_t display_color_format = 0;
static bsp_display_endianness_t   display_data_endian  = 0;
static pax_buf_t                  fb                   = {0};
static QueueHandle_t              input_event_queue    = NULL;

// ── Views ─────────────────────────────────────────────────────────────────────
typedef enum {
    VIEW_SETTINGS = 0,
    VIEW_NODES    = 1,
    VIEW_CHAT     = 2,   // DM conversations
    VIEW_CHANNEL  = 3,   // public channel (GRP_TXT)
    VIEW_COUNT    = 4,
} app_view_t;

static app_view_t current_view = VIEW_SETTINGS;

// ── LoRa RX ring buffer ───────────────────────────────────────────────────────
#define RX_BUF_SIZE 32

typedef struct {
    lora_protocol_lora_packet_t pkt;
    uint32_t                    timestamp_ms;
} rx_entry_t;

static rx_entry_t        rx_buf[RX_BUF_SIZE];
static int               rx_head    = 0;
static int               rx_count   = 0;
static SemaphoreHandle_t rx_mutex   = NULL;
static bool              lora_rx_ok = false;

// ── Node list ─────────────────────────────────────────────────────────────────
#define MAX_NODES 20

typedef struct {
    bool                   active;
    uint8_t                pub_key[MESHCORE_PUB_KEY_SIZE];
    char                   name[MESHCORE_MAX_NAME_SIZE + 1];
    meshcore_device_role_t role;
    uint32_t               last_seen_ms;
    uint16_t               packet_count;
    bool                   position_valid;
    int32_t                lat;   // degrees × 1e7
    int32_t                lon;
} node_entry_t;

static node_entry_t      node_list[MAX_NODES];
static int               node_count  = 0;
static int               node_scroll = 0;
static int               node_cursor = 0;   // index in sorted list

// DM target (set when user selects a node in Nodes tab)
static bool              dm_target_set  = false;
static uint8_t           dm_target_pub[MESHCORE_PUB_KEY_SIZE] = {0};
static char              dm_target_name[MESHCORE_MAX_NAME_SIZE + 1] = {0};
static SemaphoreHandle_t node_mutex  = NULL;

// ── Chat ──────────────────────────────────────────────────────────────────────
#define MAX_CHAT_MSGS  50
#define MAX_MSG_TEXT   172
#define MAX_INPUT_LEN  120
#define CHAT_ROW_H     38
#define CHAT_Y0        36
#define CHAT_INPUT_H   30

// Default MeshCore public channel key
static const uint8_t PUBLIC_CHANNEL_KEY[16] = {
    0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a,
    0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72
};
static uint8_t channel_hash = 0;

typedef struct {
    bool    active;
    bool    is_mine;
    char    text[MAX_MSG_TEXT];
    uint32_t timestamp_ms;
} chat_msg_t;

// DM message buffer
static chat_msg_t        chat_msgs[MAX_CHAT_MSGS];
static int               chat_head   = 0;
static int               chat_count  = 0;
static int               chat_scroll = 0;
static SemaphoreHandle_t chat_mutex  = NULL;

// Channel (GRP_TXT) message buffer
static chat_msg_t        ch_msgs[MAX_CHAT_MSGS];
static int               ch_head     = 0;
static int               ch_count    = 0;
static int               ch_scroll   = 0;
static SemaphoreHandle_t ch_mutex    = NULL;

static char chat_input[MAX_INPUT_LEN + 1] = {0};
static int  chat_input_len                = 0;
static bool chat_typing                   = false;

// ── Settings: field identifiers ───────────────────────────────────────────────
typedef enum {
    FIELD_OWNER = 0,
    FIELD_FREQ,
    FIELD_SF,
    FIELD_BW,
    FIELD_CR,
    FIELD_POWER,
    FIELD_SYNC,
    FIELD_PREAMBLE,
    FIELD_COUNT,
} field_t;

// BW options for SX1262 (kHz)
static const uint16_t BW_OPTIONS[] = {7, 10, 15, 20, 31, 41, 62, 125, 250, 500};
static const int      BW_COUNT     = (int)(sizeof(BW_OPTIONS) / sizeof(BW_OPTIONS[0]));

// NVS keys — same namespace/keys as launcher so settings are shared
#define NVS_LORA_FREQ  "lora.freq"
#define NVS_LAST_TIME  "last_time_s"  // int64: last known Unix timestamp from SNTP
#define NVS_LORA_SF    "lora.sf"
#define NVS_LORA_BW    "lora.bandwidth"
#define NVS_LORA_CR    "lora.codingrate"
#define NVS_LORA_POWER "lora.power"

// Launcher defaults (used when NVS is empty)
#define LORA_DEF_FREQ     869618000u
#define LORA_DEF_SF       8
#define LORA_DEF_BW       62
#define LORA_DEF_CR       8
#define LORA_DEF_POWER    22
#define LORA_DEF_SYNC     0x12
#define LORA_DEF_PREAMBLE 16
#define LORA_DEF_RAMP     40

// ── Node identity ─────────────────────────────────────────────────────────────
static uint8_t  node_pub_key[32]  = {0};
static uint8_t  node_prv_key[64]  = {0};
static bool     identity_ready    = false;
static uint32_t last_advert_ms    = 0;
static bool     qr_overlay_active = false;

// ── Notification LED state ────────────────────────────────────────────────────
static bool led_dm_pending      = false;
static bool led_channel_pending = false;

// ── App state ─────────────────────────────────────────────────────────────────
static int                           selected              = 0;
static bool                          edit_mode             = false;
static bool                          dirty                 = false;
static bool                          lora_ready            = false;
static bool                          c6_available          = false;
static bool                          radio_bootloader_mode = false;
static bool                          sntp_synced           = false;
static bool                          time_from_nvs         = false;
static char                          owner_name[33]        = {0};
static lora_protocol_config_params_t lora_cfg              = {0};

// ── Display helpers ───────────────────────────────────────────────────────────
static void blit(void) {
    esp_err_t res = bsp_display_blit(0, 0, display_h_res, display_v_res, pax_buf_get_pixels(&fb));
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "blit failed: %d", res);
    }
}

// ── NVS ──────────────────────────────────────────────────────────────────────
static void load_owner_name(void) {
    nvs_handle_t handle;
    esp_err_t res = nvs_open("system", NVS_READONLY, &handle);
    if (res != ESP_OK) {
        snprintf(owner_name, sizeof(owner_name), "(no NVS)");
        return;
    }
    size_t len = sizeof(owner_name) - 1;
    res = nvs_get_str(handle, "owner.nickname", owner_name, &len);
    if (res != ESP_OK) {
        snprintf(owner_name, sizeof(owner_name), "(not set)");
    }
    nvs_close(handle);
}

static void save_owner_name(void) {
    nvs_handle_t handle;
    esp_err_t res = nvs_open("system", NVS_READWRITE, &handle);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "NVS open for write failed: %d", res);
        return;
    }
    nvs_set_str(handle, "owner.nickname", owner_name);
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "Owner name saved: %s", owner_name);
}

static void load_lora_from_nvs(void) {
    lora_cfg.frequency                  = LORA_DEF_FREQ;
    lora_cfg.spreading_factor           = LORA_DEF_SF;
    lora_cfg.bandwidth                  = LORA_DEF_BW;
    lora_cfg.coding_rate                = LORA_DEF_CR;
    lora_cfg.power                      = LORA_DEF_POWER;
    lora_cfg.sync_word                  = LORA_DEF_SYNC;
    lora_cfg.preamble_length            = LORA_DEF_PREAMBLE;
    lora_cfg.ramp_time                  = LORA_DEF_RAMP;
    lora_cfg.crc_enabled                = true;
    lora_cfg.invert_iq                  = false;
    lora_cfg.low_data_rate_optimization = false;

    nvs_handle_t handle;
    if (nvs_open("system", NVS_READONLY, &handle) != ESP_OK) return;
    uint32_t freq = 0;
    uint8_t  sf = 0, cr = 0, power = 0;
    uint16_t bw = 0;
    if (nvs_get_u32(handle, NVS_LORA_FREQ,  &freq)  == ESP_OK && freq  != 0) lora_cfg.frequency        = freq;
    if (nvs_get_u8 (handle, NVS_LORA_SF,    &sf)    == ESP_OK && sf    != 0) lora_cfg.spreading_factor = sf;
    if (nvs_get_u16(handle, NVS_LORA_BW,    &bw)    == ESP_OK && bw    != 0) lora_cfg.bandwidth        = bw;
    if (nvs_get_u8 (handle, NVS_LORA_CR,    &cr)    == ESP_OK && cr    != 0) lora_cfg.coding_rate      = cr;
    if (nvs_get_u8 (handle, NVS_LORA_POWER, &power) == ESP_OK)               lora_cfg.power            = power;
    nvs_close(handle);
}

static void save_lora_to_nvs(void) {
    nvs_handle_t handle;
    if (nvs_open("system", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u32(handle, NVS_LORA_FREQ,  lora_cfg.frequency);
    nvs_set_u8 (handle, NVS_LORA_SF,   lora_cfg.spreading_factor);
    nvs_set_u16(handle, NVS_LORA_BW,   (uint16_t)lora_cfg.bandwidth);
    nvs_set_u8 (handle, NVS_LORA_CR,   lora_cfg.coding_rate);
    nvs_set_u8 (handle, NVS_LORA_POWER, lora_cfg.power);
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "LoRa config saved to NVS");
}

static void load_lora_config(void) {
    load_lora_from_nvs();
    lora_ready   = true;
    c6_available = false;

    lora_protocol_config_params_t c6_cfg = {0};
    esp_err_t res = lora_get_config(&c6_cfg);
    if (res == ESP_OK) {
        c6_available = true;
        if (c6_cfg.frequency != 0) {
            lora_cfg = c6_cfg;
            save_lora_to_nvs();
            ESP_LOGI(TAG, "LoRa config from C6: freq=%luHz sf=%d", (unsigned long)lora_cfg.frequency, lora_cfg.spreading_factor);
        } else {
            ESP_LOGI(TAG, "C6 has empty config, pushing NVS values to C6");
            lora_set_config(&lora_cfg);
        }
    } else {
        ESP_LOGW(TAG, "C6 unavailable (err=%d) — using NVS values", res);
    }
}

static void save_lora_config(void) {
    save_lora_to_nvs();
    if (c6_available) {
        esp_err_t res = lora_set_config(&lora_cfg);
        if (res != ESP_OK) {
            ESP_LOGE(TAG, "lora_set_config failed: %d", res);
        } else {
            ESP_LOGI(TAG, "LoRa config pushed to C6");
            // Re-enter RX mode — lora_set_config resets radio to standby
            if (lora_rx_ok) {
                lora_set_mode(LORA_PROTOCOL_MODE_RX);
            }
        }
    }
}

// ── Node identity ─────────────────────────────────────────────────────────────
#define NVS_IDENTITY_SEED "node.seed"

static void chat_add_message(const char* text, bool is_mine);  /* forward decl */

// ── SNTP ──────────────────────────────────────────────────────────────────────
static void sntp_sync_cb(struct timeval *tv) {
    sntp_synced = true;
    // Persist to NVS so the next boot without WiFi can restore a sane time
    nvs_handle_t h;
    if (nvs_open("system", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i64(h, NVS_LAST_TIME, (int64_t)tv->tv_sec);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void identity_init(void) {
    uint8_t seed[32] = {0};

    // Try loading existing seed from NVS
    nvs_handle_t handle;
    bool need_save = false;
    if (nvs_open("system", NVS_READWRITE, &handle) == ESP_OK) {
        size_t len = sizeof(seed);
        if (nvs_get_blob(handle, NVS_IDENTITY_SEED, seed, &len) != ESP_OK || len != 32) {
            // No seed yet — generate and save
            esp_fill_random(seed, sizeof(seed));
            nvs_set_blob(handle, NVS_IDENTITY_SEED, seed, sizeof(seed));
            need_save = true;
            ESP_LOGI(TAG, "New node identity generated");
        } else {
            ESP_LOGI(TAG, "Node identity loaded from NVS");
        }
        if (need_save) nvs_commit(handle);
        nvs_close(handle);
    } else {
        // Can't persist — use a random ephemeral identity
        esp_fill_random(seed, sizeof(seed));
        ESP_LOGW(TAG, "NVS unavailable — ephemeral identity");
    }

    ed25519_create_keypair(node_pub_key, node_prv_key, seed);
    identity_ready = true;

    // RFC 7748 X25519 test vector — verifies our Montgomery ladder
    {
        /* RFC 7748 §5 X25519 function test vector */
        static const uint8_t tv_prv[64] = {
            0xa5,0x46,0xe3,0x6b,0xf0,0x52,0x7c,0x9d,0x3b,0x16,0x15,0x4b,0x82,0x46,0x5e,0xdd,
            0x62,0x14,0x4c,0x0a,0xc1,0xfc,0x5a,0x18,0x50,0x6a,0x22,0x44,0xba,0x44,0x9a,0xc4,
            0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
        };
        static const uint8_t tv_pub[32] = {
            0xe6,0xdb,0x68,0x67,0x58,0x30,0x30,0xdb,0x35,0x94,0xc1,0xa4,0x24,0xb1,0x5f,0x7c,
            0x72,0x66,0x24,0xec,0x26,0xb3,0x35,0x3b,0x10,0xa9,0x03,0xa6,0xd0,0xab,0x1c,0x4c
        };
        static const uint8_t tv_expected[32] = {
            0xc3,0xda,0x55,0x37,0x9d,0xe9,0xc6,0x90,0x8e,0x94,0xea,0x4d,0xf2,0x8d,0x08,0x4f,
            0x32,0xec,0xcf,0x03,0x49,0x1c,0x71,0xf7,0x54,0xb4,0x07,0x55,0x77,0xa2,0x85,0x52
        };
        uint8_t tv_result[32];
        ed25519_key_exchange_raw(tv_result, tv_pub, tv_prv);
        if (memcmp(tv_result, tv_expected, 32) == 0) {
            ESP_LOGI(TAG, "X25519 RFC7748 test vector: PASS");
        } else {
            ESP_LOGE(TAG, "X25519 RFC7748 test vector: FAIL got %02X%02X%02X%02X exp %02X%02X%02X%02X",
                     tv_result[0], tv_result[1], tv_result[2], tv_result[3],
                     tv_expected[0], tv_expected[1], tv_expected[2], tv_expected[3]);
            char tv_msg[48];
            snprintf(tv_msg, sizeof(tv_msg), "X25519 tv FAIL: got %02X%02X exp %02X%02X",
                     tv_result[0], tv_result[1], tv_expected[0], tv_expected[1]);
            chat_add_message(tv_msg, false);
        }
    }

    // Self-consistency check A: conv(Ed25519 base point G) must equal u=9.
    // Tests ed25519_pub_to_x25519 independently of ge_scalarmult_base.
    {
        // Ed25519 base point G compressed (little-endian y, sign=0):
        // y_G = 0x6666...6658 (big-endian) → little-endian: byte[0]=0x58, bytes[1..31]=0x66
        static const uint8_t ed25519_G[32] = {
            0x58,0x66,0x66,0x66,0x66,0x66,0x66,0x66,
            0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,
            0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,
            0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66
        };
        static const uint8_t u9[32] = {9,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                                        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
        uint8_t conv_G[32];
        ed25519_pub_to_x25519(conv_G, ed25519_G);
        if (memcmp(conv_G, u9, 32) == 0) {
            ESP_LOGI(TAG, "conv(G)=9: PASS");
        } else {
            char msg[48];
            snprintf(msg, sizeof(msg), "conv(G)=9 FAIL got=%02X%02X%02X%02X",
                     conv_G[0], conv_G[1], conv_G[2], conv_G[3]);
            chat_add_message(msg, false);
        }
    }

    // Self-consistency check B: conv(node_pub_key) must equal X25519(our_scalar, 9).
    // If conv(G) passes but this fails → ge_scalarmult_base is wrong.
    {
        static const uint8_t base9[32] = {
            9,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
            0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0
        };
        uint8_t via_scalar[32], via_conv[32];
        ed25519_key_exchange_raw(via_scalar, base9, node_prv_key);
        ed25519_pub_to_x25519(via_conv, node_pub_key);
        if (memcmp(via_scalar, via_conv, 32) == 0) {
            ESP_LOGI(TAG, "DH self-check: PASS");
        } else {
            char sc_msg[64];
            snprintf(sc_msg, sizeof(sc_msg), "DH self-chk FAIL sc=%02X%02X cv=%02X%02X",
                     via_scalar[0], via_scalar[1], via_conv[0], via_conv[1]);
            chat_add_message(sc_msg, false);
        }
    }
    ESP_LOGI(TAG, "Pub key: "
             "%02X%02X%02X%02X%02X%02X%02X%02X"
             "%02X%02X%02X%02X%02X%02X%02X%02X"
             "%02X%02X%02X%02X%02X%02X%02X%02X"
             "%02X%02X%02X%02X%02X%02X%02X%02X",
             node_pub_key[0],  node_pub_key[1],  node_pub_key[2],  node_pub_key[3],
             node_pub_key[4],  node_pub_key[5],  node_pub_key[6],  node_pub_key[7],
             node_pub_key[8],  node_pub_key[9],  node_pub_key[10], node_pub_key[11],
             node_pub_key[12], node_pub_key[13], node_pub_key[14], node_pub_key[15],
             node_pub_key[16], node_pub_key[17], node_pub_key[18], node_pub_key[19],
             node_pub_key[20], node_pub_key[21], node_pub_key[22], node_pub_key[23],
             node_pub_key[24], node_pub_key[25], node_pub_key[26], node_pub_key[27],
             node_pub_key[28], node_pub_key[29], node_pub_key[30], node_pub_key[31]);
}

static void send_advert(void) {
    if (!c6_available || !identity_ready) return;

    meshcore_advert_t advert = {0};
    memcpy(advert.pub_key, node_pub_key, MESHCORE_PUB_KEY_SIZE);
    advert.timestamp = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS / 1000);
    advert.role      = MESHCORE_DEVICE_ROLE_CHAT_NODE;

    if (owner_name[0] && owner_name[0] != '(') {
        strncpy(advert.name, owner_name, MESHCORE_MAX_NAME_SIZE);
        advert.name_valid = true;
    }

    // Build the payload that will be signed (everything except the sig field)
    uint8_t payload[MESHCORE_MAX_PAYLOAD_SIZE];
    uint8_t payload_len = 0;
    if (meshcore_advert_serialize(&advert, payload, &payload_len) < 0) return;

    // The signature covers the bytes before the sig field (pub_key + timestamp)
    // plus the bytes after it (flags + name), i.e. everything except sig itself.
    // Serialized layout: pub_key[32] | timestamp[4] | sig[64] | flags[1] | name
    // So signed data = first 36 bytes + bytes from offset 100 onward.
    uint8_t to_sign[MESHCORE_MAX_PAYLOAD_SIZE];
    uint8_t to_sign_len = 0;
    // pub_key + timestamp (before sig)
    memcpy(to_sign, payload, MESHCORE_PUB_KEY_SIZE + 4);
    to_sign_len = MESHCORE_PUB_KEY_SIZE + 4;
    // flags + name (after sig)
    uint8_t after_sig_offset = MESHCORE_PUB_KEY_SIZE + 4 + MESHCORE_SIGNATURE_SIZE;
    if (payload_len > after_sig_offset) {
        memcpy(to_sign + to_sign_len, payload + after_sig_offset, payload_len - after_sig_offset);
        to_sign_len += payload_len - after_sig_offset;
    }

    ed25519_sign(advert.signature, to_sign, to_sign_len, node_pub_key, node_prv_key);

    // Re-serialize with the actual signature
    if (meshcore_advert_serialize(&advert, payload, &payload_len) < 0) return;

    meshcore_message_t msg = {0};
    msg.type           = MESHCORE_PAYLOAD_TYPE_ADVERT;
    msg.route          = MESHCORE_ROUTE_TYPE_FLOOD;
    msg.payload_length = payload_len;
    memcpy(msg.payload, payload, payload_len);

    uint8_t pkt_data[MESHCORE_MAX_TRANS_UNIT];
    uint8_t pkt_len = 0;
    if (meshcore_serialize(&msg, pkt_data, &pkt_len) < 0) return;

    lora_protocol_lora_packet_t pkt = {0};
    pkt.length = pkt_len;
    memcpy(pkt.data, pkt_data, pkt_len);

    lora_set_mode(LORA_PROTOCOL_MODE_TX);
    esp_err_t res = lora_send_packet(&pkt);
    lora_set_mode(LORA_PROTOCOL_MODE_RX);

    if (res == ESP_OK) {
        last_advert_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        ESP_LOGI(TAG, "ADVERT sent (%s) pub=%02X%02X%02X%02X",
                 advert.name_valid ? advert.name : "(no name)",
                 node_pub_key[0], node_pub_key[1], node_pub_key[2], node_pub_key[3]);
    } else {
        ESP_LOGE(TAG, "ADVERT send failed: %d", res);
    }
}

// ── Node list helpers ─────────────────────────────────────────────────────────
static const char* role_label(meshcore_device_role_t role) {
    switch (role) {
        case MESHCORE_DEVICE_ROLE_CHAT_NODE:   return "Chat";
        case MESHCORE_DEVICE_ROLE_REPEATER:    return "Rptr";
        case MESHCORE_DEVICE_ROLE_ROOM_SERVER: return "Room";
        case MESHCORE_DEVICE_ROLE_SENSOR:      return "Sens";
        default:                               return "?";
    }
}

// Equirectangular distance in km between two GPS positions (lat/lon in degrees × 1e7)
static float __attribute__((unused)) calc_dist_km(int32_t lat1_e7, int32_t lon1_e7, int32_t lat2_e7, int32_t lon2_e7) {
    double lat1 = lat1_e7 * 1e-7 * M_PI / 180.0;
    double lat2 = lat2_e7 * 1e-7 * M_PI / 180.0;
    double dlat = lat2 - lat1;
    double dlon = (lon2_e7 - lon1_e7) * 1e-7 * M_PI / 180.0;
    double x    = dlon * cos((lat1 + lat2) * 0.5);
    return (float)(sqrt(x * x + dlat * dlat) * 6371.0);
}

static void update_node(const meshcore_advert_t* advert, uint32_t now_ms) {
    if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;

    // Find existing node by pub_key
    int slot = -1;
    for (int i = 0; i < MAX_NODES; i++) {
        if (node_list[i].active && memcmp(node_list[i].pub_key, advert->pub_key, MESHCORE_PUB_KEY_SIZE) == 0) {
            slot = i;
            break;
        }
    }
    // Find empty slot if new
    if (slot < 0) {
        for (int i = 0; i < MAX_NODES; i++) {
            if (!node_list[i].active) { slot = i; break; }
        }
    }
    // Evict oldest if full
    if (slot < 0) {
        uint32_t oldest_ms = UINT32_MAX;
        for (int i = 0; i < MAX_NODES; i++) {
            if (node_list[i].last_seen_ms < oldest_ms) {
                oldest_ms = node_list[i].last_seen_ms;
                slot = i;
            }
        }
    }

    if (slot >= 0) {
        node_entry_t* n = &node_list[slot];
        bool is_new = !n->active;
        n->active       = true;
        n->role         = advert->role;
        n->last_seen_ms = now_ms;
        memcpy(n->pub_key, advert->pub_key, MESHCORE_PUB_KEY_SIZE);
        if (!is_new) n->packet_count++;
        else         n->packet_count = 1;
        if (advert->position_valid) {
            n->position_valid = true;
            n->lat = advert->position_lat;
            n->lon = advert->position_lon;
        }

        if (advert->name_valid && advert->name[0] != '\0') {
            strncpy(n->name, advert->name, MESHCORE_MAX_NAME_SIZE);
            n->name[MESHCORE_MAX_NAME_SIZE] = '\0';
        } else if (is_new) {
            // Use first 4 bytes of pub_key as fallback ID
            snprintf(n->name, sizeof(n->name), "%02X%02X%02X%02X",
                     advert->pub_key[0], advert->pub_key[1],
                     advert->pub_key[2], advert->pub_key[3]);
        }

        // Recalculate node_count
        node_count = 0;
        for (int i = 0; i < MAX_NODES; i++) {
            if (node_list[i].active) node_count++;
        }
        ESP_LOGI(TAG, "Node %s (%s) seen — total %d nodes", n->name, role_label(n->role), node_count);
    }

    xSemaphoreGive(node_mutex);
}

// ── Chat helpers ──────────────────────────────────────────────────────────────
static void chat_init(void) {
    // Compute channel_hash = SHA256(key)[0]
    uint8_t digest[32];
    mbedtls_sha256(PUBLIC_CHANNEL_KEY, sizeof(PUBLIC_CHANNEL_KEY), digest, 0);
    channel_hash = digest[0];
    ESP_LOGI(TAG, "Channel hash: 0x%02X", channel_hash);
}

static void chat_add_message(const char* text, bool is_mine) {
    if (xSemaphoreTake(chat_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    chat_msg_t* m    = &chat_msgs[chat_head];
    m->active        = true;
    m->is_mine       = is_mine;
    m->timestamp_ms  = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    strncpy(m->text, text, MAX_MSG_TEXT - 1);
    m->text[MAX_MSG_TEXT - 1] = '\0';
    chat_head = (chat_head + 1) % MAX_CHAT_MSGS;
    if (chat_count < MAX_CHAT_MSGS) chat_count++;
    chat_scroll = chat_count;
    xSemaphoreGive(chat_mutex);
}

static void ch_add_message(const char* text, bool is_mine) {
    if (xSemaphoreTake(ch_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    chat_msg_t* m    = &ch_msgs[ch_head];
    m->active        = true;
    m->is_mine       = is_mine;
    m->timestamp_ms  = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    strncpy(m->text, text, MAX_MSG_TEXT - 1);
    m->text[MAX_MSG_TEXT - 1] = '\0';
    ch_head = (ch_head + 1) % MAX_CHAT_MSGS;
    if (ch_count < MAX_CHAT_MSGS) ch_count++;
    ch_scroll = ch_count;
    xSemaphoreGive(ch_mutex);
}

static void update_notification_led(void) {
    tanmatsu_coprocessor_handle_t copr = NULL;
    if (bsp_tanmatsu_coprocessor_get_handle(&copr) != ESP_OK) return;
    if (led_dm_pending) {
        // Green = unread DM
        tanmatsu_coprocessor_set_message(copr, false, true, false, false, false, false, false, false);
    } else if (led_channel_pending) {
        // Blue = unread channel message
        tanmatsu_coprocessor_set_message(copr, false, false, true, false, false, false, false, false);
    } else {
        // Off
        tanmatsu_coprocessor_set_message(copr, false, false, false, false, false, false, false, false);
    }
}

static bool decrypt_grp_txt(meshcore_grp_txt_t* grp, const uint8_t* key) {
    // Verify HMAC-SHA256 (2 bytes truncated)
    uint8_t mac[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                    key, MESHCORE_CIPHER_KEY_SIZE,
                    grp->data, grp->data_length,
                    mac);
    if (memcmp(mac, grp->mac, MESHCORE_CIPHER_MAC_SIZE) != 0) return false;

    // Decrypt AES-ECB in-place
    grp->decrypted.data_length = grp->data_length;
    memcpy(grp->decrypted.data, grp->data, grp->data_length);
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_dec(&aes, key, 128);
    for (int i = 0; i < grp->decrypted.data_length / MESHCORE_CIPHER_BLOCK_SIZE; i++) {
        mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT,
                               &grp->decrypted.data[i * MESHCORE_CIPHER_BLOCK_SIZE],
                               &grp->decrypted.data[i * MESHCORE_CIPHER_BLOCK_SIZE]);
    }
    mbedtls_aes_free(&aes);

    // Parse: timestamp(4) + text_type(1) + text
    if (grp->decrypted.data_length < 5) return false;
    memcpy(&grp->decrypted.timestamp, grp->decrypted.data, 4);
    grp->decrypted.text_type = grp->decrypted.data[4];
    grp->decrypted.data[grp->decrypted.data_length - 1] = '\0';  // ensure null term
    grp->decrypted.text = (char*)&grp->decrypted.data[5];
    return true;
}

// Send an encrypted DM (TXT_MSG) to a specific node by pub_key
static bool send_dm_message(const char* text, const uint8_t* target_pub) {
    if (!c6_available || !identity_ready) return false;

    uint8_t shared[32];
    ed25519_key_exchange(shared, target_pub, node_prv_key);

    uint32_t ts = (uint32_t)time(NULL);
    uint8_t  plain[MESHCORE_MAX_PAYLOAD_SIZE] = {0};
    size_t   text_len  = strlen(text);
    size_t   plain_len = 5 + text_len;
    size_t   padded    = ((plain_len + 15) / 16) * 16;
    if (padded > MESHCORE_MAX_PAYLOAD_SIZE - 4) return false;

    memcpy(plain, &ts, 4);
    plain[4] = 0;  // flags
    memcpy(&plain[5], text, text_len);

    uint8_t cipher[MESHCORE_MAX_PAYLOAD_SIZE] = {0};
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, shared, 128);
    for (size_t i = 0; i < padded / 16; i++)
        mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, &plain[i * 16], &cipher[i * 16]);
    mbedtls_aes_free(&aes);

    uint8_t mac[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                    shared, 32, cipher, padded, mac);

    meshcore_message_t msg = {0};
    msg.type           = MESHCORE_PAYLOAD_TYPE_TXT_MSG;
    msg.route          = MESHCORE_ROUTE_TYPE_FLOOD;
    msg.version        = 0;
    msg.path_length    = 0;
    msg.payload[0]     = target_pub[0];     // dest hash
    msg.payload[1]     = node_pub_key[0];   // src hash
    msg.payload[2]     = mac[0];
    msg.payload[3]     = mac[1];
    memcpy(&msg.payload[4], cipher, padded);
    msg.payload_length = (uint8_t)(4 + padded);

    uint8_t pkt_data[MESHCORE_MAX_TRANS_UNIT];
    uint8_t pkt_len = 0;
    if (meshcore_serialize(&msg, pkt_data, &pkt_len) < 0) return false;

    lora_protocol_lora_packet_t pkt = {0};
    pkt.length = pkt_len;
    memcpy(pkt.data, pkt_data, pkt_len);
    return lora_send_packet(&pkt) == ESP_OK;
}

static bool send_chat_message(const char* text) {
    if (!c6_available) return false;

    // Build plaintext: timestamp(4) + text_type(1) + text
    uint32_t ts = (uint32_t)time(NULL);
    uint8_t  plain[MESHCORE_MAX_PAYLOAD_SIZE] = {0};
    size_t   text_len = strlen(text);
    // Include owner name prefix: "name: text"
    char prefixed[MAX_MSG_TEXT];
    if (owner_name[0] && owner_name[0] != '(') {
        snprintf(prefixed, sizeof(prefixed), "%s: %s", owner_name, text);
    } else {
        snprintf(prefixed, sizeof(prefixed), "%s", text);
    }
    text_len = strlen(prefixed);

    size_t plain_len = 4 + 1 + text_len;
    // Pad to 16-byte boundary
    size_t padded = ((plain_len + 15) / 16) * 16;
    if (padded > sizeof(plain)) return false;

    memcpy(plain, &ts, 4);
    plain[4] = 0;  // text_type = normal
    memcpy(&plain[5], prefixed, text_len);

    // Encrypt AES-ECB
    uint8_t cipher[MESHCORE_MAX_PAYLOAD_SIZE] = {0};
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, PUBLIC_CHANNEL_KEY, 128);
    for (size_t i = 0; i < padded / MESHCORE_CIPHER_BLOCK_SIZE; i++) {
        mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT,
                               &plain[i * MESHCORE_CIPHER_BLOCK_SIZE],
                               &cipher[i * MESHCORE_CIPHER_BLOCK_SIZE]);
    }
    mbedtls_aes_free(&aes);

    // HMAC-SHA256 (2 bytes)
    uint8_t mac[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                    PUBLIC_CHANNEL_KEY, MESHCORE_CIPHER_KEY_SIZE,
                    cipher, (uint16_t)padded, mac);

    // Build grp_txt payload
    meshcore_grp_txt_t grp = {0};
    grp.channel_hash = channel_hash;
    memcpy(grp.mac, mac, MESHCORE_CIPHER_MAC_SIZE);
    grp.data_length = (uint8_t)padded;
    memcpy(grp.data, cipher, padded);

    uint8_t payload[MESHCORE_MAX_PAYLOAD_SIZE];
    uint8_t payload_len = 0;
    if (meshcore_grp_txt_serialize(&grp, payload, &payload_len) < 0) return false;

    // Build meshcore message
    meshcore_message_t msg = {0};
    msg.type           = MESHCORE_PAYLOAD_TYPE_GRP_TXT;
    msg.route          = MESHCORE_ROUTE_TYPE_FLOOD;
    msg.version        = 0;
    msg.path_length    = 0;
    msg.payload_length = payload_len;
    memcpy(msg.payload, payload, payload_len);

    uint8_t pkt_data[MESHCORE_MAX_TRANS_UNIT];
    uint8_t pkt_len = 0;
    if (meshcore_serialize(&msg, pkt_data, &pkt_len) < 0) return false;

    lora_protocol_lora_packet_t pkt = {0};
    pkt.length = pkt_len;
    memcpy(pkt.data, pkt_data, pkt_len);

    // Switch to TX, send, back to RX
    lora_set_mode(LORA_PROTOCOL_MODE_TX);
    esp_err_t res = lora_send_packet(&pkt);
    lora_set_mode(LORA_PROTOCOL_MODE_RX);

    if (res == ESP_OK) {
        ESP_LOGI(TAG, "Chat sent: %s", prefixed);
        return true;
    }
    ESP_LOGE(TAG, "Chat send failed: %d", res);
    return false;
}

// ── ADVERT broadcast task ─────────────────────────────────────────────────────
#define ADVERT_INTERVAL_MS 30000  // send every 30 seconds

static void advert_task(void *arg) {
    // Initial delay so the radio is fully up before first advert
    vTaskDelay(pdMS_TO_TICKS(5000));
    while (1) {
        send_advert();
        vTaskDelay(pdMS_TO_TICKS(ADVERT_INTERVAL_MS));
    }
}

// ── LoRa RX task ─────────────────────────────────────────────────────────────
static void lora_rx_task(void *arg) {
    ESP_LOGI(TAG, "LoRa RX task started");
    while (1) {
        lora_protocol_lora_packet_t pkt = {0};
        esp_err_t res = lora_receive_packet(&pkt, pdMS_TO_TICKS(10000));
        if (res == ESP_OK && pkt.length > 0) {
            uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

            ESP_LOGI(TAG, "RX %d bytes: %02X %02X %02X %02X",
                     pkt.length,
                     pkt.length > 0 ? pkt.data[0] : 0,
                     pkt.length > 1 ? pkt.data[1] : 0,
                     pkt.length > 2 ? pkt.data[2] : 0,
                     pkt.length > 3 ? pkt.data[3] : 0);

            // Store raw packet
            if (xSemaphoreTake(rx_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                rx_buf[rx_head].pkt          = pkt;
                rx_buf[rx_head].timestamp_ms = now_ms;
                rx_head  = (rx_head + 1) % RX_BUF_SIZE;
                if (rx_count < RX_BUF_SIZE) rx_count++;
                xSemaphoreGive(rx_mutex);
            }

            // Parse MeshCore packet
            meshcore_message_t mc_msg;
            if (meshcore_deserialize(pkt.data, pkt.length, &mc_msg) >= 0) {
                if (mc_msg.type == MESHCORE_PAYLOAD_TYPE_ADVERT) {
                    meshcore_advert_t advert;
                    if (meshcore_advert_deserialize(mc_msg.payload, mc_msg.payload_length, &advert) >= 0) {
                        update_node(&advert, now_ms);
                    }
                } else if (mc_msg.type == MESHCORE_PAYLOAD_TYPE_GRP_TXT) {
                    meshcore_grp_txt_t grp = {0};
                    if (meshcore_grp_txt_deserialize(mc_msg.payload, mc_msg.payload_length, &grp) >= 0) {
                        if (decrypt_grp_txt(&grp, PUBLIC_CHANNEL_KEY)) {
                            ESP_LOGI(TAG, "Channel RX: %s", grp.decrypted.text);
                            ch_add_message(grp.decrypted.text, false);
                            if (current_view != VIEW_CHANNEL) {
                                led_channel_pending = true;
                                update_notification_led();
                            }
                        }
                    }
                } else if (mc_msg.type == MESHCORE_PAYLOAD_TYPE_TXT_MSG) {
                    // Payload: dest_hash[1] | src_hash[1] | HMAC[2] | ciphertext[...]
                    // Use do{}while(0) so 'break' exits this block, not the rx while(1)
                    do {
                        char dbg[48];
                        if (!identity_ready || mc_msg.payload_length < 6) {
                            chat_add_message("DM: not ready/short", false); break;
                        }
                        uint8_t dest_hash = mc_msg.payload[0];
                        uint8_t src_hash  = mc_msg.payload[1];

                        if (dest_hash != node_pub_key[0]) {
                            ESP_LOGD(TAG, "DM not for us (dst=%02X us=%02X)", dest_hash, node_pub_key[0]);
                            break;
                        }

                        // Find sender in node list
                        uint8_t sender_pub[32] = {0};
                        char    sender_name[MESHCORE_MAX_ADVERT_DATA_SIZE + 1] = {0};
                        bool    sender_found = false;
                        if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                            for (int ni = 0; ni < MAX_NODES; ni++) {
                                if (node_list[ni].active && node_list[ni].pub_key[0] == src_hash) {
                                    memcpy(sender_pub, node_list[ni].pub_key, 32);
                                    strncpy(sender_name, node_list[ni].name, sizeof(sender_name) - 1);
                                    sender_found = true;
                                    break;
                                }
                            }
                            xSemaphoreGive(node_mutex);
                        }
                        if (!sender_found) {
                            // Sender pubkey unknown (e.g. missed advert after reboot).
                            // Can't decrypt without pubkey — show notification, then wait
                            // for their next advert broadcast.
                            char unknown_msg[48];
                            snprintf(unknown_msg, sizeof(unknown_msg),
                                     "[?%02X] DM received (sender unknown)", src_hash);
                            chat_add_message(unknown_msg, false);
                            break;
                        }

                        // ECDH shared secret — try both with and without Edwards→Montgomery conversion
                        uint8_t secret[32];
                        ed25519_key_exchange(secret, sender_pub, node_prv_key);

                        uint8_t secret_raw[32];
                        ed25519_key_exchange_raw(secret_raw, sender_pub, node_prv_key);

                        // mac_ct = HMAC[2] | ciphertext
                        const uint8_t *mac_ct     = &mc_msg.payload[2];
                        int            mac_ct_len  = mc_msg.payload_length - 2;
                        if (mac_ct_len < MESHCORE_CIPHER_MAC_SIZE + 16) {
                            chat_add_message("DM: payload too short", false); break;
                        }

                        const uint8_t *ciphertext = mac_ct + MESHCORE_CIPHER_MAC_SIZE;
                        int            ct_len     = mac_ct_len - MESHCORE_CIPHER_MAC_SIZE;

                        // Compute HMAC for both approaches (key=32 bytes)
                        uint8_t hmac_conv[32], hmac_raw[32];
                        mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                                        secret, 32, ciphertext, ct_len, hmac_conv);
                        mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                                        secret_raw, 32, ciphertext, ct_len, hmac_raw);
                        // Also try with 16-byte HMAC key
                        uint8_t hmac_conv16[32], hmac_raw16[32];
                        mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                                        secret, 16, ciphertext, ct_len, hmac_conv16);
                        mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                                        secret_raw, 16, ciphertext, ct_len, hmac_raw16);

                        uint8_t exp0 = mac_ct[0], exp1 = mac_ct[1];
                        bool mac_ok = false;
                        uint8_t *good_secret = NULL;
                        if (hmac_conv[0]==exp0 && hmac_conv[1]==exp1)   { mac_ok=true; good_secret=secret; }
                        else if (hmac_raw[0]==exp0 && hmac_raw[1]==exp1) { mac_ok=true; good_secret=secret_raw; }
                        else if (hmac_conv16[0]==exp0 && hmac_conv16[1]==exp1) { mac_ok=true; good_secret=secret; }
                        else if (hmac_raw16[0]==exp0 && hmac_raw16[1]==exp1)   { mac_ok=true; good_secret=secret_raw; }

                        if (!mac_ok) {
                            ESP_LOGW(TAG, "DM HMAC mismatch from %02X — wrong key or unsupported variant", src_hash);
                            snprintf(dbg, sizeof(dbg), "[?%02X] DM decrypt failed", src_hash);
                            chat_add_message(dbg, false);
                            break;
                        }

                        // AES-128-ECB decrypt (key = good_secret[0..15])
                        uint8_t plaintext[MESHCORE_MAX_PAYLOAD_SIZE] = {0};
                        mbedtls_aes_context aes_ctx;
                        mbedtls_aes_init(&aes_ctx);
                        mbedtls_aes_setkey_dec(&aes_ctx, good_secret, 128);
                        for (int bi = 0; bi + 16 <= ct_len; bi += 16) {
                            mbedtls_aes_crypt_ecb(&aes_ctx, MBEDTLS_AES_DECRYPT,
                                                  ciphertext + bi, plaintext + bi);
                        }
                        mbedtls_aes_free(&aes_ctx);

                        // plaintext: timestamp[4] | flags[1] | text[...]
                        int text_len = ct_len - 5;
                        if (text_len <= 0) { chat_add_message("DM: no text", false); break; }
                        plaintext[5 + text_len - 1] = '\0';
                        char *dm_text = (char *)&plaintext[5];

                        char display[256];
                        if (sender_name[0])
                            snprintf(display, sizeof(display), "[%s] %s", sender_name, dm_text);
                        else
                            snprintf(display, sizeof(display), "[?%02X] %s", src_hash, dm_text);

                        ESP_LOGI(TAG, "DM RX: %s", display);
                        chat_add_message(display, false);
                        if (current_view != VIEW_CHAT) {
                            led_dm_pending = true;
                            update_notification_led();
                        }

                        // Send PATH_RETURN with embedded ACK (createPathReturn approach)
                        // For FLOOD DMs, MeshCore sends PAYLOAD_TYPE_PATH (0x08), not a bare ACK.
                        // Inner data (16 bytes, AES-ECB encrypted):
                        //   path_len=0 | PAYLOAD_TYPE_ACK | ack_crc[4] | padding[10]
                        // Outer payload:
                        //   dest_hash[1] | src_hash[1] | HMAC[2] | ciphertext[16]
                        {
                            // 1. Compute ACK CRC
                            uint8_t sha_out[32];
                            {
                                mbedtls_sha256_context sha_ctx;
                                mbedtls_sha256_init(&sha_ctx);
                                mbedtls_sha256_starts(&sha_ctx, 0);
                                mbedtls_sha256_update(&sha_ctx, plaintext, 5);
                                mbedtls_sha256_update(&sha_ctx, (uint8_t*)dm_text, strlen(dm_text));
                                mbedtls_sha256_update(&sha_ctx, sender_pub, 32);
                                mbedtls_sha256_finish(&sha_ctx, sha_out);
                                mbedtls_sha256_free(&sha_ctx);
                            }

                            // 2. Build and encrypt inner data
                            uint8_t inner[16] = {0};
                            inner[0] = 0x00;                        // path_len = 0
                            inner[1] = MESHCORE_PAYLOAD_TYPE_ACK;   // = 0x03
                            inner[2] = sha_out[0];
                            inner[3] = sha_out[1];
                            inner[4] = sha_out[2];
                            inner[5] = sha_out[3];
                            // inner[6..15] = 0 (zero padding for AES block)

                            uint8_t path_cipher[16];
                            {
                                mbedtls_aes_context aes;
                                mbedtls_aes_init(&aes);
                                mbedtls_aes_setkey_enc(&aes, good_secret, 128);
                                mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, inner, path_cipher);
                                mbedtls_aes_free(&aes);
                            }

                            // 3. MAC = HMAC-SHA256(good_secret[32], ciphertext)[0:2]
                            uint8_t path_mac[32];
                            mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                                            good_secret, 32, path_cipher, 16, path_mac);

                            // 4. Build PATH packet
                            meshcore_message_t path_msg = {0};
                            path_msg.type          = MESHCORE_PAYLOAD_TYPE_PATH;
                            path_msg.route         = MESHCORE_ROUTE_TYPE_FLOOD;
                            path_msg.version       = 0;
                            path_msg.path_length   = 0;
                            path_msg.payload[0]    = src_hash;        // dest = iPhone hash
                            path_msg.payload[1]    = node_pub_key[0]; // src  = our hash
                            path_msg.payload[2]    = path_mac[0];
                            path_msg.payload[3]    = path_mac[1];
                            memcpy(&path_msg.payload[4], path_cipher, 16);
                            path_msg.payload_length = 20;

                            uint8_t path_data[MESHCORE_MAX_TRANS_UNIT];
                            uint8_t path_sz = 0;
                            if (meshcore_serialize(&path_msg, path_data, &path_sz) == 0) {
                                lora_protocol_lora_packet_t lora_pkt = {0};
                                lora_pkt.length = path_sz;
                                memcpy(lora_pkt.data, path_data, path_sz);
                                lora_send_packet(&lora_pkt);
                            }
                        }
                    } while (0);
                }
            }
        }
    }
}

// ── Settings helpers ──────────────────────────────────────────────────────────
static int bw_index(void) {
    for (int i = 0; i < BW_COUNT; i++) {
        if (BW_OPTIONS[i] == (uint16_t)lora_cfg.bandwidth) return i;
    }
    return 7;
}

static void field_adjust(int field, int delta) {
    switch (field) {
        case FIELD_FREQ:
            if (delta > 0 && lora_cfg.frequency < 870000000u) lora_cfg.frequency += 100000;
            if (delta < 0 && lora_cfg.frequency > 863000000u) lora_cfg.frequency -= 100000;
            break;
        case FIELD_SF: {
            int sf = (int)lora_cfg.spreading_factor + delta;
            if (sf < 5)  sf = 5;
            if (sf > 12) sf = 12;
            lora_cfg.spreading_factor = (uint8_t)sf;
            break;
        }
        case FIELD_BW: {
            int idx = bw_index() + delta;
            if (idx < 0) idx = 0;
            if (idx >= BW_COUNT) idx = BW_COUNT - 1;
            lora_cfg.bandwidth = BW_OPTIONS[idx];
            break;
        }
        case FIELD_CR: {
            int cr = (int)lora_cfg.coding_rate + delta;
            if (cr < 5) cr = 5;
            if (cr > 8) cr = 8;
            lora_cfg.coding_rate = (uint8_t)cr;
            break;
        }
        case FIELD_POWER: {
            int p = (int)lora_cfg.power + delta;
            if (p < 2)  p = 2;
            if (p > 22) p = 22;
            lora_cfg.power = (uint8_t)p;
            break;
        }
        case FIELD_SYNC:
            lora_cfg.sync_word = (uint8_t)((lora_cfg.sync_word + delta) & 0xFF);
            break;
        case FIELD_PREAMBLE: {
            int pre = (int)lora_cfg.preamble_length + delta;
            if (pre < 2)     pre = 2;
            if (pre > 65535) pre = 65535;
            lora_cfg.preamble_length = (uint16_t)pre;
            break;
        }
        default:
            break;
    }
    dirty = true;
}

// ── Render helpers ────────────────────────────────────────────────────────────
static void render(void);

static void render_tab_bar(void) {
    int w = (int)pax_buf_get_width(&fb);
    static const char *tab_labels[VIEW_COUNT] = {"Settings", "Nodes", "DM", "Channel"};
    int tab_w = (w - 170) / VIEW_COUNT;  // reserve 170px right for status indicators

    pax_simple_rect(&fb, COL_DARK, 0, 0, w, 32);

    for (int i = 0; i < VIEW_COUNT; i++) {
        bool active = (i == (int)current_view);
        if (active) {
            pax_simple_rect(&fb, COL_ACCENT, i * tab_w, 0, tab_w, 32);
        }
        pax_col_t col = active ? COL_BLACK : COL_GRAY;
        pax_vec2f sz  = pax_text_size(pax_font_sky_mono, 16, tab_labels[i]);
        int       tx  = i * tab_w + (tab_w - (int)sz.x) / 2;
        pax_draw_text(&fb, col, pax_font_sky_mono, 16, tx, 8, tab_labels[i]);
    }

    // Right side: battery % + RX count
    char status_right[32] = {0};
    int  status_x         = w - 6;

    // Battery
    bsp_power_battery_information_t bat = {0};
    if (bsp_power_get_battery_information(&bat) == ESP_OK && bat.battery_available) {
        int pct = (int)bat.remaining_percentage;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        const char *chr = bat.battery_charging ? "+" : "";
        snprintf(status_right, sizeof(status_right), "B:%d%%%s", pct, chr);
        pax_col_t bat_col = pct <= 20 ? COL_RED : (pct <= 50 ? COL_YELLOW : COL_GREEN);
        pax_vec2f sz = pax_text_size(pax_font_sky_mono, 13, status_right);
        status_x -= (int)sz.x;
        pax_draw_text(&fb, bat_col, pax_font_sky_mono, 13, status_x, 9, status_right);
        // Separator
        status_x -= 8;
        pax_draw_text(&fb, COL_GRAY, pax_font_sky_mono, 13, status_x - 4, 9, "|");
        status_x -= 12;
    }

    // RX count
    if (lora_rx_ok) {
        int cnt = 0;
        if (xSemaphoreTake(rx_mutex, 0) == pdTRUE) {
            cnt = rx_count;
            xSemaphoreGive(rx_mutex);
        }
        char rxbuf[12];
        snprintf(rxbuf, sizeof(rxbuf), "RX:%d", cnt);
        pax_vec2f sz = pax_text_size(pax_font_sky_mono, 13, rxbuf);
        status_x -= (int)sz.x;
        pax_draw_text(&fb, COL_GREEN, pax_font_sky_mono, 13, status_x, 9, rxbuf);
    }
}

static void start_radio_bootloader(void) {
#if defined(CONFIG_IDF_TARGET_ESP32P4)
    esp_hosted_configure_heartbeat(false, 1);
    esp_hosted_deinit();
    vTaskDelay(pdMS_TO_TICKS(200));
#endif
    bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_OFF);
    vTaskDelay(pdMS_TO_TICKS(100));
    bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_BOOTLOADER);
    vTaskDelay(pdMS_TO_TICKS(500));
    render();
}

static void enter_radio_bootloader(void) {
    pax_background(&fb, COL_BLACK);
    pax_draw_text(&fb, COL_WHITE, pax_font_sky_mono, 18, 10, 10, "Radio Firmware Update");
    pax_draw_text(&fb, COL_GRAY,  pax_font_sky_mono, 14, 10, 48, "Stopping ESP-Hosted...");
    blit();
    radio_bootloader_mode = true;
    start_radio_bootloader();
}

// ── Render: bootloader screen ─────────────────────────────────────────────────
static void render_bootloader(void) {
    int w = (int)pax_buf_get_width(&fb);
    int h = (int)pax_buf_get_height(&fb);
    pax_background(&fb, COL_BLACK);
    pax_simple_rect(&fb, COL_ACCENT, 0, 0, w, 32);
    pax_draw_text(&fb, COL_BLACK, pax_font_sky_mono, 18, 10, 7, "Radio Bootloader Mode");
    int y = 44;
    pax_draw_text(&fb, COL_GREEN,  pax_font_sky_mono, 14, 10, y, "C6 is in bootloader mode."); y += 22;
    pax_draw_text(&fb, COL_WHITE,  pax_font_sky_mono, 14, 10, y, "On your Mac:"); y += 20;
    pax_draw_text(&fb, COL_YELLOW, pax_font_sky_mono, 13, 10, y, "ls /dev/cu.usbmodem*"); y += 18;
    pax_draw_text(&fb, COL_GRAY,   pax_font_sky_mono, 13, 10, y, "(find the new C6 USB device)"); y += 22;
    pax_draw_text(&fb, COL_WHITE,  pax_font_sky_mono, 13, 10, y, "cd tanmatsu-radio"); y += 18;
    pax_draw_text(&fb, COL_YELLOW, pax_font_sky_mono, 12, 10, y, "esptool.py --chip esp32c6"); y += 16;
    pax_draw_text(&fb, COL_YELLOW, pax_font_sky_mono, 12, 10, y, "  --port /dev/cu.usbmodem21401"); y += 16;
    pax_draw_text(&fb, COL_YELLOW, pax_font_sky_mono, 12, 10, y, "  --before no_reset write_flash"); y += 16;
    pax_draw_text(&fb, COL_YELLOW, pax_font_sky_mono, 12, 10, y, "  --flash_mode dio --flash_freq 80m"); y += 16;
    pax_draw_text(&fb, COL_YELLOW, pax_font_sky_mono, 12, 10, y, "  --flash_size 8MB"); y += 16;
    pax_draw_text(&fb, COL_YELLOW, pax_font_sky_mono, 12, 10, y, "  0x0 bootloader.bin 0x8000 pt.bin"); y += 16;
    pax_draw_text(&fb, COL_YELLOW, pax_font_sky_mono, 12, 10, y, "  0xd000 ota.bin 0x10000 app.bin"); y += 4;
    int fy = h - 26;
    pax_simple_rect(&fb, COL_DARK, 0, fy, w, 26);
    pax_draw_text(&fb, COL_GRAY, pax_font_sky_mono, 14, 8, fy + 6, "ESC/F1: restart badge after flashing");
    blit();
}

// ── Render: settings screen ───────────────────────────────────────────────────
static void render_settings(void) {
    int w = (int)pax_buf_get_width(&fb);
    int h = (int)pax_buf_get_height(&fb);

    pax_background(&fb, COL_BLACK);
    render_tab_bar();

    // Edit/dirty indicator in tab bar right area
    const char *mode_str = edit_mode ? "[EDIT]" : "";
    if (edit_mode) {
        pax_vec2f sz = pax_text_size(pax_font_sky_mono, 14, mode_str);
        pax_draw_text(&fb, COL_YELLOW, pax_font_sky_mono, 14, w - (int)sz.x - 80, 9, mode_str);
    }

    typedef struct { const char *label; char value[64]; } row_t;
    row_t rows[FIELD_COUNT];

    snprintf(rows[FIELD_OWNER].value, sizeof(rows[FIELD_OWNER].value), "%s", owner_name);
    rows[FIELD_OWNER].label = "Owner name";

    rows[FIELD_FREQ].label = "Frequency";
    snprintf(rows[FIELD_FREQ].value, sizeof(rows[FIELD_FREQ].value),
             "%.3f MHz", (double)lora_cfg.frequency / 1000000.0);

    rows[FIELD_SF].label = "Spreading factor";
    snprintf(rows[FIELD_SF].value, sizeof(rows[FIELD_SF].value), "SF%d", lora_cfg.spreading_factor);

    rows[FIELD_BW].label = "Bandwidth";
    snprintf(rows[FIELD_BW].value, sizeof(rows[FIELD_BW].value), "%d kHz", (int)lora_cfg.bandwidth);

    rows[FIELD_CR].label = "Coding rate";
    snprintf(rows[FIELD_CR].value, sizeof(rows[FIELD_CR].value), "4/%d", lora_cfg.coding_rate);

    rows[FIELD_POWER].label = "TX power";
    snprintf(rows[FIELD_POWER].value, sizeof(rows[FIELD_POWER].value), "%d dBm", (int)lora_cfg.power);

    rows[FIELD_SYNC].label = "Sync word";
    snprintf(rows[FIELD_SYNC].value, sizeof(rows[FIELD_SYNC].value), "0x%02X", (unsigned)lora_cfg.sync_word);

    rows[FIELD_PREAMBLE].label = "Preamble length";
    snprintf(rows[FIELD_PREAMBLE].value, sizeof(rows[FIELD_PREAMBLE].value), "%d", (int)lora_cfg.preamble_length);

    int row_h = (h - 32 - 40) / FIELD_COUNT;
    int y0    = 34;

    for (int i = 0; i < FIELD_COUNT; i++) {
        int  y      = y0 + i * row_h;
        bool is_sel = (i == selected);

        if (is_sel) {
            pax_col_t bg  = edit_mode ? 0xFF2A1A00 : 0xFF001122;
            pax_col_t bar = edit_mode ? COL_YELLOW  : COL_ACCENT;
            pax_simple_rect(&fb, bg,  0, y, w, row_h - 1);
            pax_simple_rect(&fb, bar, 0, y, 4, row_h - 1);
        }

        pax_simple_rect(&fb, COL_DARK, 4, y + row_h - 1, w - 4, 1);

        pax_col_t lbl_col = is_sel ? COL_WHITE : COL_GRAY;
        pax_draw_text(&fb, lbl_col, pax_font_sky_mono, 16, 14, y + (row_h - 16) / 2, rows[i].label);

        pax_col_t val_col;
        if (i >= FIELD_FREQ && !c6_available) {
            val_col = COL_YELLOW;
        } else if (is_sel && edit_mode) {
            val_col = COL_YELLOW;
        } else if (is_sel) {
            val_col = COL_WHITE;
        } else {
            val_col = COL_GREEN;
        }

        char val_disp[80];
        if (is_sel && edit_mode && i != FIELD_OWNER) {
            snprintf(val_disp, sizeof(val_disp), "< %s >", rows[i].value);
        } else {
            snprintf(val_disp, sizeof(val_disp), "%s", rows[i].value);
        }
        pax_vec2f vsz = pax_text_size(pax_font_sky_mono, 16, val_disp);
        pax_draw_text(&fb, val_col, pax_font_sky_mono, 16, w - (int)vsz.x - 14, y + (row_h - 16) / 2, val_disp);
    }

    int fy = h - 40;
    pax_simple_rect(&fb, COL_DARK, 0, fy, w, 40);
    if (edit_mode && selected != FIELD_OWNER) {
        pax_draw_text(&fb, COL_GRAY, pax_font_sky_mono, 14, 8, fy + 4,
                      "Up/Down or W/S: adjust  Enter: save  ESC: cancel");
    } else if (!c6_available) {
        pax_draw_text(&fb, COL_YELLOW, pax_font_sky_mono, 14, 8, fy + 4,
                      "NVS only - C6 unavailable  U: flash radio");
    } else {
        pax_draw_text(&fb, COL_GRAY, pax_font_sky_mono, 14, 8, fy + 4,
                      "W/S: navigate  Enter: edit  R: reload  Tab: next");
    }
    if (dirty) {
        pax_draw_text(&fb, COL_YELLOW, pax_font_sky_mono, 14, w - 110, fy + 4, "* unsaved");
    }

    // SNTP / time status line
    {
        time_t    now = time(NULL);
        struct tm t;
        localtime_r(&now, &t);
        char ts[48];
        if (sntp_synced) {
            snprintf(ts, sizeof(ts), "SNTP: %02d:%02d:%02d  %02d-%02d-%04d",
                     t.tm_hour, t.tm_min, t.tm_sec,
                     t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
            pax_draw_text(&fb, COL_GREEN, pax_font_sky_mono, 13, 8, fy + 24, ts);
        } else if (time_from_nvs) {
            snprintf(ts, sizeof(ts), "time: ~%02d:%02d %02d-%02d (NVS, approx)",
                     t.tm_hour, t.tm_min, t.tm_mday, t.tm_mon + 1);
            pax_draw_text(&fb, COL_YELLOW, pax_font_sky_mono, 13, 8, fy + 24, ts);
        } else {
            pax_draw_text(&fb, COL_RED, pax_font_sky_mono, 13, 8, fy + 24,
                          "time: no sync — msg timestamps incorrect");
        }
    }

    blit();
}

// ── Render: QR overlay ────────────────────────────────────────────────────────
static void render_qr_overlay(void) {
    int w = (int)pax_buf_get_width(&fb);
    int h = (int)pax_buf_get_height(&fb);

    // Build URL
    char hex_key[65];
    for (int i = 0; i < 32; i++) snprintf(&hex_key[i * 2], 3, "%02x", node_pub_key[i]);
    hex_key[64] = '\0';

    // URL-encode owner name (replace spaces with +)
    char encoded_name[64];
    int ei = 0;
    for (int i = 0; owner_name[i] && ei < 62; i++) {
        char c = owner_name[i];
        if (c == ' ') { encoded_name[ei++] = '+'; }
        else          { encoded_name[ei++] = c; }
    }
    encoded_name[ei] = '\0';

    char url[200];
    snprintf(url, sizeof(url),
             "meshcore://contact/add?name=%s&public_key=%s&type=1",
             encoded_name, hex_key);

    // Generate QR code — static buffers to avoid stack overflow (~3900 bytes each)
    static uint8_t qr_data[qrcodegen_BUFFER_LEN_MAX];
    static uint8_t tmp_buf[qrcodegen_BUFFER_LEN_MAX];
    bool ok = qrcodegen_encodeText(url, tmp_buf, qr_data,
                                   qrcodegen_Ecc_MEDIUM,
                                   qrcodegen_VERSION_MIN, 10,
                                   qrcodegen_Mask_AUTO, true);

    // Black background
    pax_background(&fb, COL_BLACK);

    if (!ok) {
        pax_draw_text(&fb, COL_YELLOW, pax_font_sky_mono, 16, 20, h / 2, "QR encode failed");
        blit();
        return;
    }

    int qr_size = qrcodegen_getSize(qr_data);
    // Scale to ~60% of screen height, centered
    int max_px  = (h * 6) / 10;
    int cell_px = max_px / qr_size;
    if (cell_px < 2) cell_px = 2;
    int qr_px   = cell_px * qr_size;
    int qr_x    = (w - qr_px) / 2;
    int qr_y    = (h - qr_px) / 2;

    // White background with quiet zone
    int margin = cell_px * 2;
    pax_simple_rect(&fb, 0xFFFFFFFF,
                    qr_x - margin, qr_y - margin,
                    qr_px + margin * 2, qr_px + margin * 2);

    // Draw modules
    for (int row = 0; row < qr_size; row++) {
        for (int col = 0; col < qr_size; col++) {
            if (qrcodegen_getModule(qr_data, col, row)) {
                pax_simple_rect(&fb, 0xFF000000,
                                qr_x + col * cell_px,
                                qr_y + row * cell_px,
                                cell_px, cell_px);
            }
        }
    }

    // Label above QR
    const char *label = "Scan to add contact";
    pax_vec2f lsz = pax_text_size(pax_font_sky_mono, 16, label);
    pax_draw_text(&fb, COL_WHITE, pax_font_sky_mono, 16,
                  (w - (int)lsz.x) / 2, qr_y - margin - 22, label);

    // Name below QR
    char name_label[80];
    snprintf(name_label, sizeof(name_label), "%s  [press any key to close]", owner_name);
    pax_vec2f nsz = pax_text_size(pax_font_sky_mono, 14, name_label);
    pax_draw_text(&fb, COL_GRAY, pax_font_sky_mono, 14,
                  (w - (int)nsz.x) / 2, qr_y + qr_px + margin + 4, name_label);

    blit();
}

// ── Render: nodes screen ──────────────────────────────────────────────────────
#define NODES_ROW_H    36
#define NODES_Y0       36
#define NODES_HEADER_H 20

static void render_nodes(void) {
    int w = (int)pax_buf_get_width(&fb);
    int h = (int)pax_buf_get_height(&fb);

    pax_background(&fb, COL_BLACK);
    render_tab_bar();

    // Column header
    int hdr_y = NODES_Y0;
    pax_simple_rect(&fb, 0xFF1A1A1A, 0, hdr_y, w, NODES_HEADER_H);
    pax_simple_rect(&fb, COL_DARK,   0, hdr_y + NODES_HEADER_H - 1, w, 1);
    // Right-side widths (mirrored from row render)
    // age_x = w - age_sz.x - 6  (variable) → use fixed col widths for header
    int age_col_w  = 52;   // "999h"
    int dist_col_w = 52;   // "999km"
    int pkts_col_w = 38;   // "#999"
    int age_hdr_x  = w - age_col_w - 4;
    int dist_hdr_x = age_hdr_x - dist_col_w;
    int pkts_hdr_x = dist_hdr_x - pkts_col_w;
    pax_draw_text(&fb, COL_GRAY, pax_font_sky_mono, 11,  4,          hdr_y + 5, "Role");
    pax_draw_text(&fb, COL_GRAY, pax_font_sky_mono, 11, 68,          hdr_y + 5, "Name");
    pax_draw_text(&fb, COL_GRAY, pax_font_sky_mono, 11, pkts_hdr_x, hdr_y + 5, "#Pkt");
    pax_draw_text(&fb, COL_GRAY, pax_font_sky_mono, 11, dist_hdr_x, hdr_y + 5, "Dist");
    pax_draw_text(&fb, COL_GRAY, pax_font_sky_mono, 11, age_hdr_x,  hdr_y + 5, "Seen");

    int list_y0   = NODES_Y0 + NODES_HEADER_H;
    int list_h    = h - 28 - list_y0;
    int rows_vis  = list_h / NODES_ROW_H;
    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    if (!lora_rx_ok) {
        pax_draw_text(&fb, COL_YELLOW, pax_font_sky_mono, 16, 10, list_y0 + 10, "LoRa radio not available");
    } else if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (node_count == 0) {
            pax_draw_text(&fb, COL_GRAY, pax_font_sky_mono, 15, 10, list_y0 + 10, "Listening... no nodes heard yet.");
        } else {
            // Clamp scroll
            int max_scroll = node_count - rows_vis;
            if (max_scroll < 0) max_scroll = 0;
            if (node_scroll > max_scroll) node_scroll = max_scroll;
            if (node_scroll < 0)         node_scroll = 0;

            // Build sorted view (by last_seen descending)
            int indices[MAX_NODES];
            int idx_count = 0;
            for (int i = 0; i < MAX_NODES; i++) {
                if (node_list[i].active) indices[idx_count++] = i;
            }
            // Simple insertion sort by last_seen_ms descending
            for (int i = 1; i < idx_count; i++) {
                int key = indices[i];
                int j   = i - 1;
                while (j >= 0 && node_list[indices[j]].last_seen_ms < node_list[key].last_seen_ms) {
                    indices[j + 1] = indices[j];
                    j--;
                }
                indices[j + 1] = key;
            }

            // Clamp cursor
            if (node_cursor >= idx_count) node_cursor = idx_count > 0 ? idx_count - 1 : 0;
            if (node_cursor < 0)          node_cursor = 0;
            // Keep cursor visible
            if (node_cursor < node_scroll)              node_scroll = node_cursor;
            if (node_cursor >= node_scroll + rows_vis)  node_scroll = node_cursor - rows_vis + 1;

            for (int row = 0; row < rows_vis; row++) {
                int list_idx = row + node_scroll;
                if (list_idx >= idx_count) break;
                node_entry_t* n = &node_list[indices[list_idx]];

                int y = list_y0 + row * NODES_ROW_H;
                bool is_cursor = (list_idx == node_cursor);

                // Row background: cursor = accent-dark, alternating for others
                if (is_cursor)         pax_simple_rect(&fb, 0xFF003366, 0, y, w, NODES_ROW_H);
                else if (row % 2 == 0) pax_simple_rect(&fb, 0xFF111111, 0, y, w, NODES_ROW_H);

                // Fixed column positions (must match header)
                int age_x  = w - age_col_w  - 4;
                int dist_x = age_x - dist_col_w;
                int pkts_x = dist_x - pkts_col_w;

                // Last seen
                uint32_t age_s = (now_ms - n->last_seen_ms) / 1000;
                char age_buf[20];
                if (age_s < 60)        snprintf(age_buf, sizeof(age_buf), "%lus", (unsigned long)age_s);
                else if (age_s < 3600) snprintf(age_buf, sizeof(age_buf), "%lum", (unsigned long)(age_s / 60));
                else                   snprintf(age_buf, sizeof(age_buf), "%luh", (unsigned long)(age_s / 3600));
                pax_draw_text(&fb, COL_GRAY, pax_font_sky_mono, 12, age_x, y + 6, age_buf);

                // Distance column
                char dist_buf[12] = "--";
                pax_draw_text(&fb, COL_GRAY, pax_font_sky_mono, 12, dist_x, y + 6, dist_buf);

                // Packet count
                char pkts_buf[8];
                snprintf(pkts_buf, sizeof(pkts_buf), "#%d", n->packet_count);
                pax_draw_text(&fb, COL_GRAY, pax_font_sky_mono, 12, pkts_x, y + 6, pkts_buf);

                // Role badge
                const char* rl = role_label(n->role);
                pax_col_t role_col = (n->role == MESHCORE_DEVICE_ROLE_REPEATER)   ? COL_ACCENT :
                                     (n->role == MESHCORE_DEVICE_ROLE_ROOM_SERVER) ? 0xFFAA44FF :
                                     (n->role == MESHCORE_DEVICE_ROLE_SENSOR)      ? COL_YELLOW :
                                                                                     COL_GREEN;
                pax_draw_text(&fb, role_col, pax_font_sky_mono, 12, 4, y + 6, rl);

                // Node name (truncated to fit between role and dist columns)
                char name_trunc[17];
                int  max_name_w = pkts_x - 72;  // pixels available for name
                // Truncate name to fit: ~8px per char at size 13
                int max_chars = max_name_w / 8;
                if (max_chars > 16) max_chars = 16;
                if (max_chars < 1)  max_chars = 1;
                snprintf(name_trunc, sizeof(name_trunc), "%.*s", max_chars, n->name);
                pax_col_t name_col = is_cursor ? COL_WHITE : 0xFFCCCCCC;
                pax_draw_text(&fb, name_col, pax_font_sky_mono, 13, 68, y + 5, name_trunc);

                // Row separator
                pax_simple_rect(&fb, COL_DARK, 0, y + NODES_ROW_H - 1, w, 1);
            }

            // Scroll indicator
            if (node_count > rows_vis) {
                char sc[24];
                snprintf(sc, sizeof(sc), "%d/%d", node_scroll + 1, node_count);
                pax_draw_text(&fb, COL_GRAY, pax_font_sky_mono, 12, w - 50, h - 28 - 16, sc);
            }
        }
        xSemaphoreGive(node_mutex);
    }

    // Two-line footer
    int footer_h = 28;
    int fy_base  = h - footer_h;
    pax_simple_rect(&fb, COL_DARK, 0, fy_base, w, footer_h);

    // Line 1: controls
    char footer_ctrl[80];
    snprintf(footer_ctrl, sizeof(footer_ctrl), "Nodes:%d  W/S:scroll  A:advert  Q:QR code  Tab:next", node_count);
    pax_draw_text(&fb, COL_GRAY, pax_font_sky_mono, 12, 8, fy_base + 1, footer_ctrl);

    // Line 2: advert age
    if (identity_ready) {
        uint32_t now_ms2 = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        char adv_buf[48];
        if (last_advert_ms == 0) {
            snprintf(adv_buf, sizeof(adv_buf), "advert: pending");
        } else {
            uint32_t age_s = (now_ms2 - last_advert_ms) / 1000;
            snprintf(adv_buf, sizeof(adv_buf), "advert: %lus ago", (unsigned long)age_s);
        }
        pax_draw_text(&fb, COL_GRAY, pax_font_sky_mono, 12, 8, fy_base + 14, adv_buf);
    }
    blit();
}

// ── Render: chat screen ───────────────────────────────────────────────────────
static void render_chat(void) {
    int w  = (int)pax_buf_get_width(&fb);
    int h  = (int)pax_buf_get_height(&fb);
    int fy = h - CHAT_INPUT_H - 26;  // footer above input bar

    pax_background(&fb, COL_BLACK);
    render_tab_bar();

    int list_h   = fy - CHAT_Y0;
    int rows_vis = list_h / CHAT_ROW_H;

    if (xSemaphoreTake(chat_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (chat_count == 0) {
            pax_draw_text(&fb, COL_GRAY, pax_font_sky_mono, 14, 10, CHAT_Y0 + 10,
                          "No messages yet. T to type.");
        } else {
            // Clamp scroll: 0 = top, chat_count = bottom (newest visible)
            int max_scroll = chat_count;
            if (chat_scroll > max_scroll) chat_scroll = max_scroll;
            if (chat_scroll < rows_vis)   chat_scroll = rows_vis;

            // Show rows_vis messages ending at chat_scroll
            for (int row = 0; row < rows_vis; row++) {
                int msg_idx_in_list = chat_scroll - rows_vis + row;
                if (msg_idx_in_list < 0) continue;

                // Map to ring buffer
                int ring_idx = (chat_head - chat_count + msg_idx_in_list + MAX_CHAT_MSGS * 2) % MAX_CHAT_MSGS;
                chat_msg_t* m = &chat_msgs[ring_idx];
                if (!m->active) continue;

                int y = CHAT_Y0 + row * CHAT_ROW_H;

                if (m->is_mine) {
                    // My messages right-aligned, accent color
                    pax_vec2f sz = pax_text_size(pax_font_sky_mono, 14, m->text);
                    int tx = w - (int)sz.x - 10;
                    if (tx < 10) tx = 10;
                    pax_draw_text(&fb, COL_ACCENT, pax_font_sky_mono, 14, tx, y + 4, m->text);
                    pax_draw_text(&fb, COL_GRAY, pax_font_sky_mono, 12, tx, y + 20, "You");
                } else {
                    // Incoming messages left-aligned
                    pax_draw_text(&fb, COL_WHITE, pax_font_sky_mono, 14, 10, y + 4, m->text);
                }
                pax_simple_rect(&fb, COL_DARK, 4, y + CHAT_ROW_H - 1, w - 8, 1);
            }
        }
        xSemaphoreGive(chat_mutex);
    }

    // Input bar
    int iy = h - CHAT_INPUT_H - 22;
    pax_simple_rect(&fb, 0xFF1A1A1A, 0, iy, w, CHAT_INPUT_H);
    pax_col_t bar_border = dm_target_set ? COL_YELLOW : (chat_typing ? COL_ACCENT : COL_DARK);
    pax_simple_rect(&fb, bar_border, 0, iy, w, 1);
    if (chat_typing) {
        char prefix[MESHCORE_MAX_NAME_SIZE + 8];
        if (dm_target_set)
            snprintf(prefix, sizeof(prefix), "DM %s> ", dm_target_name);
        else
            snprintf(prefix, sizeof(prefix), "> ");
        char disp[MAX_INPUT_LEN + sizeof(prefix) + 2];
        snprintf(disp, sizeof(disp), "%s%s_", prefix, chat_input);
        pax_draw_text(&fb, COL_WHITE, pax_font_sky_mono, 15, 8, iy + 7, disp);
        // Character counter (right side)
        char ctr[12];
        snprintf(ctr, sizeof(ctr), "%d/%d", chat_input_len, MAX_INPUT_LEN);
        pax_vec2f ctr_sz = pax_text_size(pax_font_sky_mono, 12, ctr);
        pax_draw_text(&fb, COL_GRAY, pax_font_sky_mono, 12, w - (int)ctr_sz.x - 6, iy + 9, ctr);
    } else {
        if (dm_target_set) {
            char hint[MESHCORE_MAX_NAME_SIZE + 32];
            snprintf(hint, sizeof(hint), "T: DM %s  |  ESC: channel", dm_target_name);
            pax_draw_text(&fb, COL_YELLOW, pax_font_sky_mono, 13, 8, iy + 9, hint);
        } else {
            pax_draw_text(&fb, COL_GRAY, pax_font_sky_mono, 14, 8, iy + 8, "T: type  |  Nodes: pick DM");
        }
    }

    // Footer
    int footer_y = h - 22;
    pax_simple_rect(&fb, COL_DARK, 0, footer_y, w, 22);
    if (chat_typing) {
        pax_draw_text(&fb, COL_GRAY, pax_font_sky_mono, 13, 8, footer_y + 4,
                      "Enter: send  ESC: cancel  Backspace: delete");
    } else {
        pax_draw_text(&fb, COL_GRAY, pax_font_sky_mono, 13, 8, footer_y + 4,
                      "T: type  W/S: scroll  Tab: next  ESC: exit");
    }
    blit();
}

// ── Render: channel screen ────────────────────────────────────────────────────
static void render_channel(void) {
    int w  = (int)pax_buf_get_width(&fb);
    int h  = (int)pax_buf_get_height(&fb);
    int fy = h - CHAT_INPUT_H - 26;

    pax_background(&fb, COL_BLACK);
    render_tab_bar();

    int list_h   = fy - CHAT_Y0;
    int rows_vis = list_h / CHAT_ROW_H;

    if (xSemaphoreTake(ch_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (ch_count == 0) {
            pax_draw_text(&fb, COL_GRAY, pax_font_sky_mono, 14, 10, CHAT_Y0 + 10,
                          "No channel messages yet. T to send.");
        } else {
            int max_scroll = ch_count;
            if (ch_scroll > max_scroll) ch_scroll = max_scroll;
            if (ch_scroll < rows_vis)   ch_scroll = rows_vis;

            for (int row = 0; row < rows_vis; row++) {
                int msg_idx = ch_scroll - rows_vis + row;
                if (msg_idx < 0) continue;
                int ring_idx = (ch_head - ch_count + msg_idx + MAX_CHAT_MSGS * 2) % MAX_CHAT_MSGS;
                chat_msg_t* m = &ch_msgs[ring_idx];
                if (!m->active) continue;

                int y = CHAT_Y0 + row * CHAT_ROW_H;
                if (m->is_mine) {
                    pax_vec2f sz = pax_text_size(pax_font_sky_mono, 14, m->text);
                    int tx = w - (int)sz.x - 10;
                    if (tx < 10) tx = 10;
                    pax_draw_text(&fb, COL_ACCENT, pax_font_sky_mono, 14, tx, y + 4, m->text);
                    pax_draw_text(&fb, COL_GRAY, pax_font_sky_mono, 12, tx, y + 20, "You");
                } else {
                    pax_draw_text(&fb, COL_WHITE, pax_font_sky_mono, 14, 10, y + 4, m->text);
                }
                pax_simple_rect(&fb, COL_DARK, 4, y + CHAT_ROW_H - 1, w - 8, 1);
            }
        }
        xSemaphoreGive(ch_mutex);
    }

    // Input bar
    int iy = h - CHAT_INPUT_H - 22;
    pax_simple_rect(&fb, 0xFF1A1A1A, 0, iy, w, CHAT_INPUT_H);
    pax_simple_rect(&fb, chat_typing ? COL_GREEN : COL_DARK, 0, iy, w, 1);
    if (chat_typing) {
        char disp[MAX_INPUT_LEN + 4];
        snprintf(disp, sizeof(disp), "> %s_", chat_input);
        pax_draw_text(&fb, COL_WHITE, pax_font_sky_mono, 15, 8, iy + 7, disp);
        char ctr[12];
        snprintf(ctr, sizeof(ctr), "%d/%d", chat_input_len, MAX_INPUT_LEN);
        pax_vec2f ctr_sz = pax_text_size(pax_font_sky_mono, 12, ctr);
        pax_draw_text(&fb, COL_GRAY, pax_font_sky_mono, 12, w - (int)ctr_sz.x - 6, iy + 9, ctr);
    } else {
        pax_draw_text(&fb, COL_GRAY, pax_font_sky_mono, 14, 8, iy + 8, "T: send channel message");
    }

    int footer_y = h - 22;
    pax_simple_rect(&fb, COL_DARK, 0, footer_y, w, 22);
    if (chat_typing)
        pax_draw_text(&fb, COL_GRAY, pax_font_sky_mono, 13, 8, footer_y + 4,
                      "Enter: send  ESC: cancel  Backspace: delete");
    else
        pax_draw_text(&fb, COL_GRAY, pax_font_sky_mono, 13, 8, footer_y + 4,
                      "T: type  W/S: scroll  Tab: next  ESC: exit");
    blit();
}

// ── Render dispatcher ─────────────────────────────────────────────────────────
static void render(void) {
    if (radio_bootloader_mode) {
        render_bootloader();
        return;
    }
    switch (current_view) {
        case VIEW_NODES:
            render_nodes();
            if (qr_overlay_active) render_qr_overlay();
            break;
        case VIEW_CHAT:    render_chat();    break;
        case VIEW_CHANNEL: render_channel(); break;
        case VIEW_SETTINGS:
        default:           render_settings(); break;
    }
}

// ── Input handling ────────────────────────────────────────────────────────────
static void handle_nav(uint32_t key) {
    if (radio_bootloader_mode) {
        if (key == BSP_INPUT_NAVIGATION_KEY_F1 || key == BSP_INPUT_NAVIGATION_KEY_ESC) {
            bsp_led_set_mode(true);
            bsp_device_restart_to_launcher();
        }
        return;
    }

    // QR overlay: any nav key closes it
    if (qr_overlay_active) {
        qr_overlay_active = false;
        return;
    }

    if (key == BSP_INPUT_NAVIGATION_KEY_F1 || key == BSP_INPUT_NAVIGATION_KEY_ESC) {
        if (edit_mode) {
            edit_mode = false;
            dirty     = false;
            load_owner_name();
            load_lora_config();
        } else if (current_view == VIEW_CHAT && dm_target_set) {
            dm_target_set = false;
        } else {
            bsp_led_set_mode(true);
            bsp_device_restart_to_launcher();
        }
    } else if (key == BSP_INPUT_NAVIGATION_KEY_UP) {
        if (current_view == VIEW_SETTINGS) {
            if (!edit_mode) selected = (selected - 1 + FIELD_COUNT) % FIELD_COUNT;
            else if (selected != FIELD_OWNER) field_adjust(selected, +1);
        } else if (current_view == VIEW_NODES) {
            if (node_cursor > 0) node_cursor--;
        }
    } else if (key == BSP_INPUT_NAVIGATION_KEY_DOWN) {
        if (current_view == VIEW_SETTINGS) {
            if (!edit_mode) selected = (selected + 1) % FIELD_COUNT;
            else if (selected != FIELD_OWNER) field_adjust(selected, -1);
        } else if (current_view == VIEW_NODES) {
            if (node_cursor < node_count - 1) node_cursor++;
        }
    } else if (key == BSP_INPUT_NAVIGATION_KEY_LEFT) {
        if (current_view == VIEW_SETTINGS && edit_mode && selected != FIELD_OWNER) field_adjust(selected, -1);
    } else if (key == BSP_INPUT_NAVIGATION_KEY_RIGHT) {
        if (current_view == VIEW_SETTINGS && edit_mode && selected != FIELD_OWNER) field_adjust(selected, +1);
    } else if (key == BSP_INPUT_NAVIGATION_KEY_RETURN) {
        if ((current_view == VIEW_CHAT || current_view == VIEW_CHANNEL) && chat_typing) {
            if (chat_input_len > 0) {
                if (current_view == VIEW_CHANNEL) {
                    send_chat_message(chat_input);
                    ch_add_message(chat_input, true);
                } else if (dm_target_set) {
                    send_dm_message(chat_input, dm_target_pub);
                    chat_add_message(chat_input, true);
                } else {
                    send_chat_message(chat_input);
                    chat_add_message(chat_input, true);
                }
                chat_input_len = 0;
                chat_input[0]  = '\0';
            }
            chat_typing = false;
        } else if (current_view == VIEW_NODES) {
            // Select node under cursor → set DM target, open Chat tab
            if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                int indices[MAX_NODES];
                int idx_count = 0;
                for (int i = 0; i < MAX_NODES; i++)
                    if (node_list[i].active) indices[idx_count++] = i;
                for (int i = 1; i < idx_count; i++) {
                    int key2 = indices[i], j = i - 1;
                    while (j >= 0 && node_list[indices[j]].last_seen_ms < node_list[key2].last_seen_ms)
                        { indices[j + 1] = indices[j]; j--; }
                    indices[j + 1] = key2;
                }
                if (node_cursor < idx_count) {
                    node_entry_t* n = &node_list[indices[node_cursor]];
                    dm_target_set = true;
                    memcpy(dm_target_pub, n->pub_key, MESHCORE_PUB_KEY_SIZE);
                    strncpy(dm_target_name, n->name, sizeof(dm_target_name) - 1);
                    dm_target_name[sizeof(dm_target_name) - 1] = '\0';
                }
                xSemaphoreGive(node_mutex);
            }
            if (dm_target_set) { current_view = VIEW_CHAT; led_dm_pending = false; update_notification_led(); }
        } else if (current_view == VIEW_SETTINGS) {
            if (!edit_mode) {
                edit_mode = true;
            } else {
                if (selected == FIELD_OWNER) save_owner_name();
                else save_lora_config();
                edit_mode = false;
                dirty     = false;
            }
        }
    }
}

static void handle_key(char c) {
    if (radio_bootloader_mode) {
        if (c == 27) {
            bsp_led_set_mode(true);
            bsp_device_restart_to_launcher();
        }
        return;
    }

    // QR overlay: ESC closes it, all other keys ignored
    if (qr_overlay_active) {
        if (c == 27) qr_overlay_active = false;
        return;
    }

    // Chat / Channel view input — intercept everything when typing
    if (current_view == VIEW_CHAT || current_view == VIEW_CHANNEL) {
        if (chat_typing) {
            if (c == 27) {  // ESC: cancel
                chat_typing    = false;
                chat_input_len = 0;
                chat_input[0]  = '\0';
            } else if (c == '\r' || c == '\n') {  // Enter: send
                if (chat_input_len > 0) {
                    if (current_view == VIEW_CHANNEL) {
                        send_chat_message(chat_input);
                        ch_add_message(chat_input, true);
                    } else if (dm_target_set) {
                        send_dm_message(chat_input, dm_target_pub);
                        chat_add_message(chat_input, true);
                    } else {
                        send_chat_message(chat_input);
                        chat_add_message(chat_input, true);
                    }
                    chat_input_len = 0;
                    chat_input[0]  = '\0';
                }
                chat_typing = false;
            } else if (c == 127 || c == 8) {  // Backspace
                if (chat_input_len > 0) {
                    chat_input[--chat_input_len] = '\0';
                }
            } else if (c >= 32 && c < 127 && chat_input_len < MAX_INPUT_LEN) {
                chat_input[chat_input_len++] = c;
                chat_input[chat_input_len]   = '\0';
            }
            return;
        } else {
            if (c == 't' || c == 'T') { chat_typing = true; return; }
            if (c == 'w' || c == 'W') {
                if (current_view == VIEW_CHANNEL) { if (ch_scroll > 0) ch_scroll--; }
                else                              { if (chat_scroll > 0) chat_scroll--; }
                return;
            }
            if (c == 's' || c == 'S') {
                if (current_view == VIEW_CHANNEL) ch_scroll++;
                else                              chat_scroll++;
                return;
            }
            // Tab and ESC fall through to common handling below
        }
    }

    if (c == '\t') {
        // Tab: cycle through views (not in edit mode)
        if (!edit_mode) {
            current_view = (app_view_t)((int)(current_view + 1) % VIEW_COUNT);
            if (current_view == VIEW_CHAT)    { led_dm_pending     = false; update_notification_led(); }
            if (current_view == VIEW_CHANNEL) { led_channel_pending = false; update_notification_led(); }
        }
        return;
    }

    if (c == 27) {
        if (edit_mode) {
            edit_mode = false;
            dirty     = false;
            load_owner_name();
            load_lora_config();
        } else if (current_view == VIEW_CHAT && dm_target_set) {
            dm_target_set = false;  // clear DM target, back to channel
        } else {
            bsp_led_set_mode(true);
            bsp_device_restart_to_launcher();
        }
    } else if (c == 'w' || c == 'W') {
        if (current_view == VIEW_SETTINGS) {
            if (!edit_mode) selected = (selected - 1 + FIELD_COUNT) % FIELD_COUNT;
            else if (selected != FIELD_OWNER) field_adjust(selected, +1);
        } else if (current_view == VIEW_NODES) {
            if (node_cursor > 0) node_cursor--;
        }
    } else if (c == 's' || c == 'S') {
        if (current_view == VIEW_SETTINGS) {
            if (!edit_mode) selected = (selected + 1) % FIELD_COUNT;
            else if (selected != FIELD_OWNER) field_adjust(selected, -1);
        } else if (current_view == VIEW_NODES) {
            if (node_cursor < node_count - 1) node_cursor++;
        }
    } else if ((c == 'a' || c == 'A') && current_view == VIEW_NODES) {
        send_advert();
    } else if ((c == 'q' || c == 'Q') && current_view == VIEW_NODES && identity_ready) {
        qr_overlay_active = true;
    } else if (c == '<' || c == ',') {
        if (current_view == VIEW_SETTINGS && edit_mode && selected != FIELD_OWNER) field_adjust(selected, -1);
    } else if (c == '>' || c == '.') {
        if (current_view == VIEW_SETTINGS && edit_mode && selected != FIELD_OWNER) field_adjust(selected, +1);
    } else if (c == '\r' || c == '\n') {
        if (current_view == VIEW_NODES) {
            // Select node under cursor → open DM in Chat tab
            if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                int indices[MAX_NODES];
                int idx_count = 0;
                for (int i = 0; i < MAX_NODES; i++)
                    if (node_list[i].active) indices[idx_count++] = i;
                for (int i = 1; i < idx_count; i++) {
                    int key = indices[i], j = i - 1;
                    while (j >= 0 && node_list[indices[j]].last_seen_ms < node_list[key].last_seen_ms)
                        { indices[j + 1] = indices[j]; j--; }
                    indices[j + 1] = key;
                }
                if (node_cursor < idx_count) {
                    node_entry_t* n = &node_list[indices[node_cursor]];
                    dm_target_set = true;
                    memcpy(dm_target_pub, n->pub_key, MESHCORE_PUB_KEY_SIZE);
                    strncpy(dm_target_name, n->name, sizeof(dm_target_name) - 1);
                    dm_target_name[sizeof(dm_target_name) - 1] = '\0';
                }
                xSemaphoreGive(node_mutex);
            }
            if (dm_target_set) { current_view = VIEW_CHAT; led_dm_pending = false; update_notification_led(); }
        } else if (current_view == VIEW_SETTINGS) {
            if (!edit_mode) {
                edit_mode = true;
            } else {
                if (selected == FIELD_OWNER) save_owner_name();
                else save_lora_config();
                edit_mode = false;
                dirty     = false;
            }
        }
    } else if (c == 'r' || c == 'R') {
        if (current_view == VIEW_SETTINGS) {
            load_owner_name();
            load_lora_config();
            dirty     = false;
            edit_mode = false;
        }
        // R on any view forces an immediate re-render (already happens via changed=true)
    } else if ((c == 'u' || c == 'U') && !c6_available && !edit_mode && current_view == VIEW_SETTINGS) {
        enter_radio_bootloader();
    }
}

// ── app_main ──────────────────────────────────────────────────────────────────
void app_main(void) {
    gpio_install_isr_service(0);

    esp_err_t res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES || res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        res = nvs_flash_init();
    }
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %d", res);
        return;
    }

    const bsp_configuration_t bsp_cfg = {
        .display = {
            .requested_color_format = BSP_DISPLAY_COLOR_FORMAT_24_888RGB,
            .num_fbs                = 1,
        },
    };
    res = bsp_device_initialize(&bsp_cfg);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "BSP init failed: %d", res);
        return;
    }

    res = bsp_display_get_parameters(&display_h_res, &display_v_res, &display_color_format, &display_data_endian);
    if (res != ESP_ERR_NOT_SUPPORTED && res == ESP_OK) {
        pax_buf_type_t fmt = PAX_BUF_24_888RGB;
        switch (display_color_format) {
            case BSP_DISPLAY_COLOR_FORMAT_16_565RGB:   fmt = PAX_BUF_16_565RGB;   break;
            case BSP_DISPLAY_COLOR_FORMAT_32_8888ARGB: fmt = PAX_BUF_32_8888ARGB; break;
            default: break;
        }
        bsp_display_rotation_t rot = bsp_display_get_default_rotation();
        pax_orientation_t ori = PAX_O_UPRIGHT;
        switch (rot) {
            case BSP_DISPLAY_ROTATION_90:  ori = PAX_O_ROT_CCW;  break;
            case BSP_DISPLAY_ROTATION_180: ori = PAX_O_ROT_HALF; break;
            case BSP_DISPLAY_ROTATION_270: ori = PAX_O_ROT_CW;   break;
            default: break;
        }
        pax_buf_init(&fb, NULL, display_h_res, display_v_res, fmt);
        pax_buf_reversed(&fb, display_data_endian == BSP_DISPLAY_ENDIAN_BIG);
        pax_buf_set_orientation(&fb, ori);
    }

    ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));

    // Mutexes
    rx_mutex   = xSemaphoreCreateMutex();
    node_mutex = xSemaphoreCreateMutex();
    chat_mutex = xSemaphoreCreateMutex();
    ch_mutex   = xSemaphoreCreateMutex();
    memset(node_list,  0, sizeof(node_list));
    memset(ch_msgs,    0, sizeof(ch_msgs));
    memset(chat_msgs,  0, sizeof(chat_msgs));
    chat_init();
    identity_init();

    int  diag_y    = 40;
    int  diag_line = 20;
#define DIAG(col, fmt, ...) do { \
        char _buf[80]; \
        snprintf(_buf, sizeof(_buf), fmt, ##__VA_ARGS__); \
        ESP_LOGI(TAG, "%s", _buf); \
        pax_draw_text(&fb, (col), pax_font_sky_mono, 14, 10, diag_y, _buf); \
        diag_y += diag_line; \
        blit(); \
    } while(0)

    pax_background(&fb, COL_RED);
    pax_draw_text(&fb, COL_WHITE, pax_font_sky_mono, 18, 10, 10, "MeshCore v4");
    blit();
    vTaskDelay(pdMS_TO_TICKS(1500));

    DIAG(COL_GRAY, "wifi_connection_init_stack...");
    res = wifi_connection_init_stack();
    DIAG(res == ESP_OK ? COL_GREEN : COL_YELLOW, "  wifi init: %s (%d)",
         res == ESP_OK ? "OK" : "FAIL", res);
    if (res == ESP_OK) {
        res = wifi_connect_try_all();
        DIAG(res == ESP_OK ? COL_GREEN : COL_YELLOW, "  wifi connect: %s",
             res == ESP_OK ? "OK" : "no saved networks");
        if (res == ESP_OK) {
            esp_sntp_set_time_sync_notification_cb(sntp_sync_cb);
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_init();
        }
    }

    // Restore last known time from NVS when no WiFi/SNTP available
    if (!sntp_synced) {
        nvs_handle_t h;
        if (nvs_open("system", NVS_READONLY, &h) == ESP_OK) {
            int64_t saved = 0;
            if (nvs_get_i64(h, NVS_LAST_TIME, &saved) == ESP_OK && saved > 1000000000LL) {
                struct timeval tv = { .tv_sec = (time_t)saved, .tv_usec = 0 };
                settimeofday(&tv, NULL);
                time_from_nvs = true;
                DIAG(COL_YELLOW, "  time: NVS restore (approx)");
            }
            nvs_close(h);
        }
    }

    load_owner_name();

    DIAG(COL_GRAY, "lora_init(16)...");
    res = lora_init(16);
    DIAG(res == ESP_OK ? COL_GREEN : COL_RED, "  lora_init: %s (%d)",
         res == ESP_OK ? "OK" : "FAIL", res);

    load_lora_from_nvs();
    lora_ready = true;
    DIAG(COL_GREEN, "NVS: %.3fMHz SF%d BW%d",
         (double)lora_cfg.frequency / 1000000.0, lora_cfg.spreading_factor, (int)lora_cfg.bandwidth);

    if (res == ESP_OK) {
        DIAG(COL_GRAY, "lora_get_config from C6...");
        lora_protocol_config_params_t c6_cfg = {0};
        esp_err_t cfg_res = lora_get_config(&c6_cfg);
        if (cfg_res == ESP_OK) {
            c6_available = true;
            if (c6_cfg.frequency != 0) {
                lora_cfg = c6_cfg;
                save_lora_to_nvs();
                DIAG(COL_GREEN, "  C6 OK! %.3fMHz SF%d",
                     (double)lora_cfg.frequency / 1000000.0, lora_cfg.spreading_factor);
            } else {
                DIAG(COL_YELLOW, "  C6 fresh - pushing NVS config");
                lora_set_config(&lora_cfg);
            }

            // Set RX mode and start background task
            DIAG(COL_GRAY, "lora_set_mode(RX)...");
            esp_err_t mode_res = lora_set_mode(LORA_PROTOCOL_MODE_RX);
            if (mode_res == ESP_OK) {
                lora_rx_ok = true;
                DIAG(COL_GREEN, "  RX mode OK - starting tasks");
                xTaskCreate(lora_rx_task, "lora_rx",    10240, NULL, 5, NULL);
                xTaskCreate(advert_task,  "lora_advert", 6144, NULL, 4, NULL);
            } else {
                DIAG(COL_YELLOW, "  RX mode failed (%d)", mode_res);
            }
        } else {
            DIAG(COL_YELLOW, "  C6 unavail (err=%d) - NVS only", cfg_res);
        }
    } else {
        DIAG(COL_YELLOW, "lora_init failed - NVS values only");
    }

    vTaskDelay(pdMS_TO_TICKS(3000));
#undef DIAG

    render();

    while (1) {
        bsp_input_event_t event;
        if (xQueueReceive(input_event_queue, &event, pdMS_TO_TICKS(1000)) != pdTRUE) {
            render();  // periodic refresh: update RX count, last-seen timers
            continue;
        }

        bool changed = false;

        if (event.type == INPUT_EVENT_TYPE_NAVIGATION && event.args_navigation.state) {
            handle_nav(event.args_navigation.key);
            changed = true;
        } else if (event.type == INPUT_EVENT_TYPE_KEYBOARD) {
            handle_key(event.args_keyboard.ascii);
            changed = true;
        }

        if (changed) {
            render();
        }
    }
}
