#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "bsp/led.h"
#include "bsp/power.h"
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
    VIEW_CHAT     = 2,
    VIEW_COUNT    = 3,
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
} node_entry_t;

static node_entry_t      node_list[MAX_NODES];
static int               node_count  = 0;
static int               node_scroll = 0;
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

static chat_msg_t        chat_msgs[MAX_CHAT_MSGS];
static int               chat_head   = 0;
static int               chat_count  = 0;
static int               chat_scroll = 0;
static SemaphoreHandle_t chat_mutex  = NULL;

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

// ── App state ─────────────────────────────────────────────────────────────────
static int                           selected              = 0;
static bool                          edit_mode             = false;
static bool                          dirty                 = false;
static bool                          lora_ready            = false;
static bool                          c6_available          = false;
static bool                          radio_bootloader_mode = false;
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
    // Auto-scroll to bottom
    chat_scroll = chat_count;
    xSemaphoreGive(chat_mutex);
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

static bool send_chat_message(const char* text) {
    if (!c6_available) return false;

    // Build plaintext: timestamp(4) + text_type(1) + text
    uint32_t ts = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS / 1000);
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
                            ESP_LOGI(TAG, "Chat RX: %s", grp.decrypted.text);
                            chat_add_message(grp.decrypted.text, false);
                        }
                    }
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
    static const char *tab_labels[VIEW_COUNT] = {"Settings", "Nodes", "Chat"};
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

    int row_h = (h - 32 - 28) / FIELD_COUNT;
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

    int fy = h - 26;
    pax_simple_rect(&fb, COL_DARK, 0, fy, w, 26);
    if (edit_mode && selected != FIELD_OWNER) {
        pax_draw_text(&fb, COL_GRAY, pax_font_sky_mono, 14, 8, fy + 6,
                      "Up/Down or W/S: adjust  Enter: save  ESC: cancel");
    } else if (!c6_available) {
        pax_draw_text(&fb, COL_YELLOW, pax_font_sky_mono, 14, 8, fy + 6,
                      "NVS only - C6 unavailable  U: flash radio");
    } else {
        pax_draw_text(&fb, COL_GRAY, pax_font_sky_mono, 14, 8, fy + 6,
                      "W/S: navigate  Enter: edit  R: reload  Tab: next");
    }
    if (dirty) {
        pax_draw_text(&fb, COL_YELLOW, pax_font_sky_mono, 14, w - 110, fy + 6, "* unsaved");
    }

    blit();
}

// ── Render: nodes screen ──────────────────────────────────────────────────────
#define NODES_ROW_H  36
#define NODES_Y0     36

static void render_nodes(void) {
    int w = (int)pax_buf_get_width(&fb);
    int h = (int)pax_buf_get_height(&fb);

    pax_background(&fb, COL_BLACK);
    render_tab_bar();

    int fy        = h - 26;
    int list_h    = fy - NODES_Y0;
    int rows_vis  = list_h / NODES_ROW_H;
    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    if (!lora_rx_ok) {
        pax_draw_text(&fb, COL_YELLOW, pax_font_sky_mono, 16, 10, NODES_Y0 + 10, "LoRa radio not available");
    } else if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (node_count == 0) {
            pax_draw_text(&fb, COL_GRAY, pax_font_sky_mono, 15, 10, NODES_Y0 + 10, "Listening... no nodes heard yet.");
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

            for (int row = 0; row < rows_vis; row++) {
                int list_idx = row + node_scroll;
                if (list_idx >= idx_count) break;
                node_entry_t* n = &node_list[indices[list_idx]];

                int y = NODES_Y0 + row * NODES_ROW_H;

                // Alternating row background
                if (row % 2 == 0) pax_simple_rect(&fb, 0xFF111111, 0, y, w, NODES_ROW_H);

                // Role badge
                const char* rl = role_label(n->role);
                pax_col_t role_col = (n->role == MESHCORE_DEVICE_ROLE_REPEATER)    ? COL_ACCENT :
                                     (n->role == MESHCORE_DEVICE_ROLE_ROOM_SERVER)  ? 0xFFAA44FF :
                                     (n->role == MESHCORE_DEVICE_ROLE_SENSOR)       ? COL_YELLOW :
                                                                                      COL_GREEN;
                pax_draw_text(&fb, role_col, pax_font_sky_mono, 13, 8, y + 3, rl);

                // Node name
                pax_draw_text(&fb, COL_WHITE, pax_font_sky_mono, 16, 50, y + 2, n->name);

                // Last seen
                uint32_t age_s = (now_ms - n->last_seen_ms) / 1000;
                char age_buf[20];
                if (age_s < 60)        snprintf(age_buf, sizeof(age_buf), "%lus ago", (unsigned long)age_s);
                else if (age_s < 3600) snprintf(age_buf, sizeof(age_buf), "%lum ago", (unsigned long)(age_s / 60));
                else                   snprintf(age_buf, sizeof(age_buf), "%luh ago", (unsigned long)(age_s / 3600));
                pax_vec2f sz = pax_text_size(pax_font_sky_mono, 13, age_buf);
                pax_draw_text(&fb, COL_GRAY, pax_font_sky_mono, 13, w - (int)sz.x - 8, y + 5, age_buf);

                // Packet count
                char cnt_buf[12];
                snprintf(cnt_buf, sizeof(cnt_buf), "#%d", n->packet_count);
                pax_vec2f csz = pax_text_size(pax_font_sky_mono, 13, cnt_buf);
                pax_draw_text(&fb, COL_GRAY, pax_font_sky_mono, 13,
                              w - (int)sz.x - (int)csz.x - 16, y + 5, cnt_buf);

                // Row separator
                pax_simple_rect(&fb, COL_DARK, 0, y + NODES_ROW_H - 1, w, 1);
            }

            // Scroll indicator
            if (node_count > rows_vis) {
                char sc[24];
                snprintf(sc, sizeof(sc), "%d/%d", node_scroll + 1, node_count);
                pax_draw_text(&fb, COL_GRAY, pax_font_sky_mono, 12, w - 50, fy - 16, sc);
            }
        }
        xSemaphoreGive(node_mutex);
    }

    pax_simple_rect(&fb, COL_DARK, 0, fy, w, 26);
    char footer[56];
    snprintf(footer, sizeof(footer), "Nodes: %d  W/S: scroll  R: refresh  Tab: next", node_count);
    pax_draw_text(&fb, COL_GRAY, pax_font_sky_mono, 14, 8, fy + 6, footer);
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
    pax_simple_rect(&fb, chat_typing ? COL_ACCENT : COL_DARK, 0, iy, w, 1);
    if (chat_typing) {
        char disp[MAX_INPUT_LEN + 4];
        snprintf(disp, sizeof(disp), "> %s_", chat_input);
        pax_draw_text(&fb, COL_WHITE, pax_font_sky_mono, 15, 8, iy + 7, disp);
    } else {
        pax_draw_text(&fb, COL_GRAY, pax_font_sky_mono, 14, 8, iy + 8, "T: type a message");
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

// ── Render dispatcher ─────────────────────────────────────────────────────────
static void render(void) {
    if (radio_bootloader_mode) {
        render_bootloader();
        return;
    }
    switch (current_view) {
        case VIEW_NODES:    render_nodes();    break;
        case VIEW_CHAT:     render_chat();     break;
        case VIEW_SETTINGS:
        default:            render_settings(); break;
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

    if (key == BSP_INPUT_NAVIGATION_KEY_F1 || key == BSP_INPUT_NAVIGATION_KEY_ESC) {
        if (edit_mode) {
            edit_mode = false;
            dirty     = false;
            load_owner_name();
            load_lora_config();
        } else {
            bsp_led_set_mode(true);
            bsp_device_restart_to_launcher();
        }
    } else if (key == BSP_INPUT_NAVIGATION_KEY_UP) {
        if (current_view == VIEW_SETTINGS) {
            if (!edit_mode) selected = (selected - 1 + FIELD_COUNT) % FIELD_COUNT;
            else if (selected != FIELD_OWNER) field_adjust(selected, +1);
        }
    } else if (key == BSP_INPUT_NAVIGATION_KEY_DOWN) {
        if (current_view == VIEW_SETTINGS) {
            if (!edit_mode) selected = (selected + 1) % FIELD_COUNT;
            else if (selected != FIELD_OWNER) field_adjust(selected, -1);
        }
    } else if (key == BSP_INPUT_NAVIGATION_KEY_LEFT) {
        if (current_view == VIEW_SETTINGS && edit_mode && selected != FIELD_OWNER) field_adjust(selected, -1);
    } else if (key == BSP_INPUT_NAVIGATION_KEY_RIGHT) {
        if (current_view == VIEW_SETTINGS && edit_mode && selected != FIELD_OWNER) field_adjust(selected, +1);
    } else if (key == BSP_INPUT_NAVIGATION_KEY_RETURN) {
        if (current_view == VIEW_CHAT && chat_typing) {
            if (chat_input_len > 0) {
                send_chat_message(chat_input);      // best-effort TX
                chat_add_message(chat_input, true); // always show locally
                chat_input_len = 0;
                chat_input[0]  = '\0';
            }
            chat_typing = false;
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

    // Chat view input — intercept everything when typing
    if (current_view == VIEW_CHAT) {
        if (chat_typing) {
            if (c == 27) {  // ESC: cancel
                chat_typing    = false;
                chat_input_len = 0;
                chat_input[0]  = '\0';
            } else if (c == '\r' || c == '\n') {  // Enter: send
                if (chat_input_len > 0) {
                    send_chat_message(chat_input);      // best-effort TX
                    chat_add_message(chat_input, true); // always show locally
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
            if (c == 'w' || c == 'W') { if (chat_scroll > 0) chat_scroll--; return; }
            if (c == 's' || c == 'S') { chat_scroll++; return; }
            // Tab and ESC fall through to common handling below
        }
    }

    if (c == '\t') {
        // Tab: cycle through views (not in edit mode)
        if (!edit_mode) {
            current_view = (app_view_t)((int)(current_view + 1) % VIEW_COUNT);
        }
        return;
    }

    if (c == 27) {
        if (edit_mode) {
            edit_mode = false;
            dirty     = false;
            load_owner_name();
            load_lora_config();
        } else {
            bsp_led_set_mode(true);
            bsp_device_restart_to_launcher();
        }
    } else if (c == 'w' || c == 'W') {
        if (current_view == VIEW_SETTINGS) {
            if (!edit_mode) selected = (selected - 1 + FIELD_COUNT) % FIELD_COUNT;
            else if (selected != FIELD_OWNER) field_adjust(selected, +1);
        } else if (current_view == VIEW_NODES) {
            if (node_scroll > 0) node_scroll--;
        }
    } else if (c == 's' || c == 'S') {
        if (current_view == VIEW_SETTINGS) {
            if (!edit_mode) selected = (selected + 1) % FIELD_COUNT;
            else if (selected != FIELD_OWNER) field_adjust(selected, -1);
        } else if (current_view == VIEW_NODES) {
            node_scroll++;
        }
    } else if (c == '<' || c == ',') {
        if (current_view == VIEW_SETTINGS && edit_mode && selected != FIELD_OWNER) field_adjust(selected, -1);
    } else if (c == '>' || c == '.') {
        if (current_view == VIEW_SETTINGS && edit_mode && selected != FIELD_OWNER) field_adjust(selected, +1);
    } else if (c == '\r' || c == '\n') {
        if (current_view == VIEW_SETTINGS) {
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
    memset(node_list,  0, sizeof(node_list));
    memset(chat_msgs,  0, sizeof(chat_msgs));
    chat_init();

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
    DIAG(res == ESP_OK ? COL_GREEN : COL_RED, "  wifi init: %s (%d)",
         res == ESP_OK ? "OK" : "FAIL", res);
    if (res == ESP_OK) {
        res = wifi_connect_try_all();
        DIAG(res == ESP_OK ? COL_GREEN : COL_YELLOW, "  wifi connect: %s",
             res == ESP_OK ? "OK" : "no saved networks");
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
                DIAG(COL_GREEN, "  RX mode OK - starting task");
                xTaskCreate(lora_rx_task, "lora_rx", 4096, NULL, 5, NULL);
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
