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
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <ctype.h>
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
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include <sys/stat.h>

// Tanmatsu SD-card pins (mirrors managed_components/.../tanmatsu_hardware.h,
// which is private to the BSP target — duplicated here so we don't reach in).
#define MC_SDCARD_CLK 43
#define MC_SDCARD_CMD 44
#define MC_SDCARD_D0  39
#define MC_SDCARD_D1  40
#define MC_SDCARD_D2  41
#define MC_SDCARD_D3  42
#endif

static char const TAG[] = "main";

// ── Colors (Tokyo Night palette, matches WHY2025 cj_lora_info) ────────────────
// Existing COL_* names kept; only hex values updated so the 99 call-sites map
// to the new palette without rename churn. COL_BLACK / COL_DARK are repurposed
// as background + panel surfaces respectively.
#define COL_BLACK   0xFF1A1B26  // BG       — main background (deep indigo)
#define COL_HEADER  0xFF16161E  // HEADER   — tab bar + footer surface
#define COL_DARK    0xFF24283B  // PANEL    — row highlight / separators
#define COL_WHITE   0xFFC0CAF5  // TEXT     — body text (off-white)
#define COL_GRAY    0xFF565F89  // DIM      — secondary text
#define COL_ACCENT  0xFF7AA2F7  // BLUE     — selection / accent
#define COL_GREEN   0xFF9ECE6A  // GREEN    — ok / online
#define COL_YELLOW  0xFFE0AF68  // AMBER    — heading / warning
#define COL_RED     0xFFF7768E  // RED      — error / offline
#define COL_AMBER   COL_YELLOW   // alias for clarity in headings
#define COL_BLUE    COL_ACCENT   // alias for clarity in info rows
#define COL_PANEL   COL_DARK     // alias for clarity in surface use
#define COL_BG      COL_BLACK    // alias for clarity at root background

// ── Typography (Saira Regular: ASCII + Latin-1, variable pitch) ──────────────
#define FONT         pax_font_saira_regular
#define TXT_TITLE    24
#define TXT_TAB      22
#define TXT_BODY     20
#define TXT_SMALL    16
#define TXT_TINY     13

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

// ── LoRa RF statistics (laatste RX + noise floor) ─────────────────────────────
static volatile int8_t   last_rx_rssi_dbm        = 0;   // 0 = no data
static volatile int8_t   last_rx_snr_db_x4       = 0;   // SNR in 1/4 dB units
static volatile int8_t   last_rx_signal_rssi_dbm = 0;
static volatile uint32_t last_rx_stats_ms        = 0;
static volatile bool     last_rx_stats_valid     = false;
static volatile int8_t   noise_floor_dbm         = 0;   // 0 = no data
static volatile bool     noise_floor_valid       = false;
static volatile bool     noise_floor_supported   = true; // false na NACK van oude C6

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
    bool                   stats_valid;
    int8_t                 last_rssi_dbm;
    int8_t                 last_snr_db_x4;
} node_entry_t;

static node_entry_t      node_list[MAX_NODES];
static int               node_count  = 0;
static int               node_scroll = 0;
static int               node_cursor = 0;   // index in sorted list (contacts + live)

// ── Contacts (favorites) ──────────────────────────────────────────────────────
#define MAX_CONTACTS      16
#define CONTACT_ALIAS_LEN 24

typedef struct __attribute__((packed)) {
    uint8_t pub_key[MESHCORE_PUB_KEY_SIZE];  // 32
    char    alias[CONTACT_ALIAS_LEN];         // 24, NUL-padded
    uint8_t role;                              // meshcore_device_role_t (1)
    uint8_t flags;                             // reserved (1)
} contact_t;

static contact_t contacts[MAX_CONTACTS];
static int       contact_count = 0;

// DM target (set when user selects a node in Nodes tab)
static bool              dm_target_set  = false;
static uint8_t           dm_target_pub[MESHCORE_PUB_KEY_SIZE] = {0};
static char              dm_target_name[MESHCORE_MAX_NAME_SIZE + 1] = {0};
static SemaphoreHandle_t node_mutex  = NULL;

// ── Chat ──────────────────────────────────────────────────────────────────────
#define MAX_CHAT_MSGS  50
#define MAX_MSG_TEXT   172
#define MAX_INPUT_LEN  120
#define TAB_BAR_H      44
#define FOOTER_H       28
#define CHAT_ROW_H     44
#define CHAT_Y0        (TAB_BAR_H + 4)
#define CHAT_INPUT_H   36

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

// DM tab inbox-view state (backlog #18 — Light variant: single shared chat ring,
// inbox just lets user pick which contact to switch the dm_target to).
static bool dm_inbox_mode    = true;  // true = list of contacts; false = chat history
static int  dm_inbox_cursor  = 0;
static int  dm_inbox_scroll  = 0;

// ── Settings: field identifiers ───────────────────────────────────────────────
typedef enum {
    FIELD_OWNER = 0,
    FIELD_ADV_NAME,
    FIELD_FREQ,
    FIELD_SF,
    FIELD_BW,
    FIELD_CR,
    FIELD_POWER,
    FIELD_SYNC,
    FIELD_PREAMBLE,
    FIELD_ADVERT_INT,
    FIELD_PRESET,
    FIELD_ROLE,
    FIELD_PATH_HASH_SIZE,
    FIELD_REGION_SCOPE,
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
#define NVS_LORA_ADVERT_INT "lora.advint_s"  // u16: advert interval in seconds
#define NVS_LORA_ROLE       "lora.role"      // u8: meshcore_device_role_t
#define NVS_LORA_PATHHASH   "lora.pathhash"  // u8: 1/2/3 bytes per path-hop
#define NVS_LORA_REGION     "lora.region"    // str: lowercase region scope (e.g. "nl")

// Launcher defaults (used when NVS is empty)
#define LORA_DEF_FREQ     869618000u
#define LORA_DEF_SF       8
#define LORA_DEF_BW       62
#define LORA_DEF_CR       8
#define LORA_DEF_POWER    22
#define LORA_DEF_SYNC     0x12
#define LORA_DEF_PREAMBLE 16
#define LORA_DEF_RAMP     40
#define LORA_DEF_ADVERT_INT 300  // 5 min — reduces TX duty cycle vs old 30s
#define LORA_DEF_ROLE     1      // MESHCORE_DEVICE_ROLE_CHAT_NODE
// Default 1 byte for now — 2-byte RX is still buggy (#21). Bump to 2 once #21 is fixed.
#define LORA_DEF_PATHHASH 1

// LoRa profile presets (SF/BW/CR triplets)
typedef struct {
    const char *name;
    uint8_t     sf;
    uint16_t    bw;
    uint8_t     cr;
} lora_preset_t;

static const lora_preset_t LORA_PRESETS[] = {
    {"LR Slow",  11,  31, 8},
    {"LR Std",   10,  62, 6},
    {"MeshCore",  8,  62, 6},
    {"SR Fast",   7, 250, 5},
};
static const int LORA_PRESET_COUNT = (int)(sizeof(LORA_PRESETS) / sizeof(LORA_PRESETS[0]));

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
static char                          lora_advert_name[33]  = {0};  // overrides owner_name in advert if set
// Shared text-edit scratch for FIELD_OWNER / FIELD_ADV_NAME.
static char                          field_edit_buf[33]    = {0};
static int                           field_edit_len        = 0;
static bool                          field_editing_text    = false;
static lora_protocol_config_params_t lora_cfg              = {0};
static uint16_t                      advert_interval_s     = LORA_DEF_ADVERT_INT;
static meshcore_device_role_t        lora_role             = MESHCORE_DEVICE_ROLE_CHAT_NODE;
static uint8_t                       path_hash_size        = LORA_DEF_PATHHASH;  // 1/2/3 bytes per path-hop
static char                          region_scope[33]      = {0};                // lowercase, e.g. "nl"
static int                           settings_scroll       = 0;
// node_filter == UNKNOWN means "show all roles"
static meshcore_device_role_t        node_filter           = MESHCORE_DEVICE_ROLE_UNKNOWN;

// Replace UTF-8 multi-byte sequences (e.g. emoji) with '?' so the ASCII-only
// font renders something visible instead of garbled bytes. Continuation bytes
// are silently skipped — the lead byte emits one placeholder per sequence.
static void utf8_sanitize(char *dst, size_t dst_sz, const char *src) {
    size_t di = 0;
    for (size_t si = 0; src[si] && di + 1 < dst_sz; si++) {
        unsigned char c = (unsigned char)src[si];
        if (c < 0x80) {
            dst[di++] = (char)c;
        } else if ((c & 0xC0) == 0xC0) {
            dst[di++] = '?';
        }
    }
    dst[di] = '\0';
}

static int lora_preset_match(void) {
    for (int i = 0; i < LORA_PRESET_COUNT; i++) {
        if (LORA_PRESETS[i].sf == lora_cfg.spreading_factor &&
            LORA_PRESETS[i].bw == (uint16_t)lora_cfg.bandwidth &&
            LORA_PRESETS[i].cr == lora_cfg.coding_rate) {
            return i;
        }
    }
    return -1;
}

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

static void load_lora_advert_name(void) {
    nvs_handle_t handle;
    if (nvs_open("system", NVS_READONLY, &handle) != ESP_OK) return;
    size_t len = sizeof(lora_advert_name) - 1;
    if (nvs_get_str(handle, "lora.advname", lora_advert_name, &len) != ESP_OK) {
        lora_advert_name[0] = '\0';
    }
    nvs_close(handle);
}

static void save_lora_advert_name(void) {
    nvs_handle_t handle;
    if (nvs_open("system", NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "NVS open for write failed (lora.advname)");
        return;
    }
    // Empty string = "use owner_name as fallback" → remove the key.
    if (lora_advert_name[0] == '\0') {
        nvs_erase_key(handle, "lora.advname");
    } else {
        nvs_set_str(handle, "lora.advname", lora_advert_name);
    }
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "LoRa advert name saved: %s",
             lora_advert_name[0] ? lora_advert_name : "(cleared)");
}

static void load_region_scope(void) {
    nvs_handle_t handle;
    if (nvs_open("system", NVS_READONLY, &handle) != ESP_OK) {
        region_scope[0] = '\0';
        return;
    }
    size_t len = sizeof(region_scope) - 1;
    if (nvs_get_str(handle, NVS_LORA_REGION, region_scope, &len) != ESP_OK) {
        region_scope[0] = '\0';
    }
    nvs_close(handle);
}

static void save_region_scope(void) {
    nvs_handle_t handle;
    if (nvs_open("system", NVS_READWRITE, &handle) != ESP_OK) return;
    if (region_scope[0] == '\0') {
        nvs_erase_key(handle, NVS_LORA_REGION);
    } else {
        nvs_set_str(handle, NVS_LORA_REGION, region_scope);
    }
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "Region scope saved: %s",
             region_scope[0] ? region_scope : "(cleared)");
}

// ── Contacts NVS ──────────────────────────────────────────────────────────────
#define NVS_CONTACTS_BLOB "mc.contacts"

static void load_contacts(void) {
    contact_count = 0;
    memset(contacts, 0, sizeof(contacts));
    nvs_handle_t handle;
    if (nvs_open("system", NVS_READONLY, &handle) != ESP_OK) return;
    size_t blob_sz = sizeof(contacts);
    if (nvs_get_blob(handle, NVS_CONTACTS_BLOB, contacts, &blob_sz) == ESP_OK) {
        int n = (int)(blob_sz / sizeof(contact_t));
        if (n > MAX_CONTACTS) n = MAX_CONTACTS;
        contact_count = n;
        ESP_LOGI(TAG, "Loaded %d contact(s) from NVS", contact_count);
    }
    nvs_close(handle);
}

static void save_contacts(void) {
    nvs_handle_t handle;
    if (nvs_open("system", NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "NVS open for contacts write failed");
        return;
    }
    if (contact_count == 0) {
        nvs_erase_key(handle, NVS_CONTACTS_BLOB);
    } else {
        nvs_set_blob(handle, NVS_CONTACTS_BLOB, contacts,
                     (size_t)contact_count * sizeof(contact_t));
    }
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "Saved %d contact(s) to NVS", contact_count);
}

static int contact_find(const uint8_t *pub) {
    for (int i = 0; i < contact_count; i++) {
        if (memcmp(contacts[i].pub_key, pub, MESHCORE_PUB_KEY_SIZE) == 0) return i;
    }
    return -1;
}

// Combined display row for Nodes tab: contacts on top (possibly offline),
// then live nodes that aren't already in contacts.
typedef struct {
    bool is_contact;     // true: contact entry (may also be live via node_idx)
    int  contact_idx;    // index into contacts[]; -1 if live-only
    int  node_idx;       // index into node_list[]; -1 if offline contact
} display_row_t;

// Builds the combined view. Caller must hold node_mutex.
static int build_node_display(display_row_t *rows, int max_rows) {
    int n = 0;

    // 1) Contacts first, filtered by node_filter on the stored role
    for (int ci = 0; ci < contact_count && n < max_rows; ci++) {
        if (node_filter != MESHCORE_DEVICE_ROLE_UNKNOWN &&
            (meshcore_device_role_t)contacts[ci].role != node_filter) continue;
        int ni = -1;
        for (int j = 0; j < MAX_NODES; j++) {
            if (node_list[j].active &&
                memcmp(node_list[j].pub_key, contacts[ci].pub_key, MESHCORE_PUB_KEY_SIZE) == 0) {
                ni = j; break;
            }
        }
        rows[n].is_contact  = true;
        rows[n].contact_idx = ci;
        rows[n].node_idx    = ni;
        n++;
    }

    // 2) Live nodes (not in contacts), filtered by role, sorted by last_seen desc
    int live[MAX_NODES];
    int live_count = 0;
    for (int i = 0; i < MAX_NODES; i++) {
        if (!node_list[i].active) continue;
        if (node_filter != MESHCORE_DEVICE_ROLE_UNKNOWN &&
            node_list[i].role != node_filter) continue;
        if (contact_find(node_list[i].pub_key) >= 0) continue;
        live[live_count++] = i;
    }
    for (int i = 1; i < live_count; i++) {
        int k = live[i], j = i - 1;
        while (j >= 0 && node_list[live[j]].last_seen_ms < node_list[k].last_seen_ms) {
            live[j + 1] = live[j]; j--;
        }
        live[j + 1] = k;
    }
    for (int i = 0; i < live_count && n < max_rows; i++) {
        rows[n].is_contact  = false;
        rows[n].contact_idx = -1;
        rows[n].node_idx    = live[i];
        n++;
    }
    return n;
}

// Idempotent add — used to persist anyone we've ever DM'd with.
// Returns 1 if added, 0 if already known, -1 if full.
static int contact_ensure(const uint8_t *pub, const char *name, uint8_t role) {
    if (contact_find(pub) >= 0) return 0;
    if (contact_count >= MAX_CONTACTS) return -1;
    contact_t *c = &contacts[contact_count++];
    memcpy(c->pub_key, pub, MESHCORE_PUB_KEY_SIZE);
    strncpy(c->alias, name ? name : "", CONTACT_ALIAS_LEN - 1);
    c->alias[CONTACT_ALIAS_LEN - 1] = '\0';
    c->role  = role;
    c->flags = 0;
    save_contacts();
    return 1;
}

// Add (uses node name as alias) or remove. Returns +1 added, 0 removed, -1 full.
static int contact_toggle(const uint8_t *pub, const char *name, uint8_t role) {
    int idx = contact_find(pub);
    if (idx >= 0) {
        // Remove: shift down
        for (int i = idx; i < contact_count - 1; i++) contacts[i] = contacts[i + 1];
        contact_count--;
        memset(&contacts[contact_count], 0, sizeof(contact_t));
        save_contacts();
        return 0;
    }
    if (contact_count >= MAX_CONTACTS) return -1;
    contact_t *c = &contacts[contact_count++];
    memcpy(c->pub_key, pub, MESHCORE_PUB_KEY_SIZE);
    strncpy(c->alias, name ? name : "", CONTACT_ALIAS_LEN - 1);
    c->alias[CONTACT_ALIAS_LEN - 1] = '\0';
    c->role  = role;
    c->flags = 0;
    save_contacts();
    return 1;
}

// ── SD-card encrypted history (DM + channel, append-only) ────────────────────
#define SD_MOUNT_POINT       "/sd"
#define HISTORY_DIR          "/sd/meshcore"
#define HISTORY_DM_DIR       "/sd/meshcore/dm"
#define HISTORY_LOG_PATH     "/sd/meshcore/chat.bin"      // legacy, no longer written or read
#define CH_HISTORY_LOG_PATH  "/sd/meshcore/channel.bin"
#define HISTORY_REC_MAGIC    "MCR1"

static bool              history_ready = false;
static const char       *history_status = "off";   // "off"|"ok"|"no-sd"|"err"
static uint8_t           history_key[32] = {0};    // HMAC-SHA256 output; first 16 used for AES-128
static SemaphoreHandle_t history_mutex = NULL;
static sdmmc_card_t     *sd_card_handle = NULL;

typedef struct __attribute__((packed)) {
    uint8_t  magic[4];      // "MCR1"
    uint16_t plain_len;     // 1..MAX_MSG_TEXT
    uint8_t  flags;         // bit0 = is_mine
    uint8_t  reserved;
    uint32_t ts_unix;       // seconds since epoch (LE)
    uint8_t  iv[16];
} history_rec_hdr_t;          // 28 bytes

static esp_err_t history_mount_sd(void) {
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;  // Tanmatsu: slot 0 = µSD; slot 1 = hosted Wi-Fi link
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = MC_SDCARD_CLK;
    slot.cmd = MC_SDCARD_CMD;
    slot.d0  = MC_SDCARD_D0;
    slot.d1  = MC_SDCARD_D1;
    slot.d2  = MC_SDCARD_D2;
    slot.d3  = MC_SDCARD_D3;
    slot.width = 4;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mnt = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    return esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mnt, &sd_card_handle);
}

// Derive AES-128 key from node private key. Stable per device.
static void history_derive_key(void) {
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                    node_prv_key, 32,
                    (const uint8_t *)"mc-history-v1", 13,
                    history_key);  // 32 B HMAC; AES uses first 16 only
}

static void history_init(void) {
    history_mutex = xSemaphoreCreateMutex();

    esp_err_t e = history_mount_sd();
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s — chat history disabled", esp_err_to_name(e));
        history_status = (e == ESP_ERR_NOT_FOUND || e == ESP_ERR_TIMEOUT) ? "no-sd" : "err";
        return;
    }
    ESP_LOGI(TAG, "SD mounted at %s", SD_MOUNT_POINT);

    mkdir(HISTORY_DIR, 0775);     // ignores EEXIST
    mkdir(HISTORY_DM_DIR, 0775);  // per-contact DM history dir

    history_derive_key();
    history_ready = true;
    history_status = "ok";
}

// Generic append: writes one AES-CBC encrypted record to `path`.
// Safe to call before history_ready (becomes a no-op).
static void history_append_impl(const char *path, const char *text, bool is_mine) {
    if (!history_ready || text == NULL) return;
    int N = (int)strnlen(text, MAX_MSG_TEXT);
    if (N <= 0) return;

    if (xSemaphoreTake(history_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return;

    history_rec_hdr_t hdr;
    memcpy(hdr.magic, HISTORY_REC_MAGIC, 4);
    hdr.plain_len = (uint16_t)N;
    hdr.flags     = is_mine ? 0x01 : 0x00;
    hdr.reserved  = 0;
    hdr.ts_unix   = (uint32_t)time(NULL);
    esp_fill_random(hdr.iv, 16);

    int padded = ((N + 16) / 16) * 16;
    uint8_t pt[MAX_MSG_TEXT + 32];
    memcpy(pt, text, N);
    uint8_t pad = (uint8_t)(padded - N);
    for (int i = N; i < padded; i++) pt[i] = pad;

    uint8_t ct[MAX_MSG_TEXT + 32];
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, history_key, 128);
    uint8_t iv_copy[16];
    memcpy(iv_copy, hdr.iv, 16);
    mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, padded, iv_copy, pt, ct);
    mbedtls_aes_free(&aes);

    FILE *f = fopen(path, "ab");
    if (f) {
        fwrite(&hdr, sizeof(hdr), 1, f);
        fwrite(ct,    padded,     1, f);
        fclose(f);
    } else {
        ESP_LOGW(TAG, "history_append: fopen(%s) failed", path);
    }
    xSemaphoreGive(history_mutex);
}

// Callback receives decrypted text + flags; called from history_load_impl under history_mutex.
typedef void (*history_ring_add_fn)(const char *text, bool is_mine);

static void history_load_impl(const char *path, history_ring_add_fn add) {
    if (!history_ready) return;
    if (xSemaphoreTake(history_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return;

    FILE *f = fopen(path, "rb");
    if (!f) { xSemaphoreGive(history_mutex); return; }

    int loaded = 0;
    while (1) {
        history_rec_hdr_t hdr;
        if (fread(&hdr, sizeof(hdr), 1, f) != 1) break;
        if (memcmp(hdr.magic, HISTORY_REC_MAGIC, 4) != 0) {
            ESP_LOGW(TAG, "history(%s): bad magic at rec %d — stop", path, loaded);
            break;
        }
        if (hdr.plain_len == 0 || hdr.plain_len > MAX_MSG_TEXT) {
            ESP_LOGW(TAG, "history(%s): bad len %u at rec %d — stop", path, hdr.plain_len, loaded);
            break;
        }
        int padded = ((hdr.plain_len + 16) / 16) * 16;
        uint8_t ct[MAX_MSG_TEXT + 32];
        if (fread(ct, padded, 1, f) != 1) break;

        uint8_t pt[MAX_MSG_TEXT + 32];
        mbedtls_aes_context aes;
        mbedtls_aes_init(&aes);
        mbedtls_aes_setkey_dec(&aes, history_key, 128);
        mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, padded, hdr.iv, ct, pt);
        mbedtls_aes_free(&aes);

        uint8_t pad = pt[padded - 1];
        if (pad == 0 || pad > 16 || (padded - pad) != hdr.plain_len) {
            ESP_LOGW(TAG, "history(%s): decrypt mismatch at rec %d — stop", path, loaded);
            break;
        }
        int N = hdr.plain_len;

        char text[MAX_MSG_TEXT];
        int copy = N < MAX_MSG_TEXT - 1 ? N : MAX_MSG_TEXT - 1;
        memcpy(text, pt, copy);
        text[copy] = '\0';

        add(text, (hdr.flags & 0x01) != 0);
        loaded++;
    }
    fclose(f);

    ESP_LOGI(TAG, "history_load(%s): %d record(s) restored", path, loaded);
    xSemaphoreGive(history_mutex);
}

static void chat_ring_add_from_disk(const char *text, bool is_mine) {
    if (xSemaphoreTake(chat_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    chat_msg_t *m   = &chat_msgs[chat_head];
    m->active       = true;
    m->is_mine      = is_mine;
    m->timestamp_ms = 0;
    utf8_sanitize(m->text, MAX_MSG_TEXT, text);
    chat_head = (chat_head + 1) % MAX_CHAT_MSGS;
    if (chat_count < MAX_CHAT_MSGS) chat_count++;
    chat_scroll = chat_count;
    xSemaphoreGive(chat_mutex);
}

static void ch_ring_add_from_disk(const char *text, bool is_mine) {
    if (xSemaphoreTake(ch_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    chat_msg_t *m   = &ch_msgs[ch_head];
    m->active       = true;
    m->is_mine      = is_mine;
    m->timestamp_ms = 0;
    utf8_sanitize(m->text, MAX_MSG_TEXT, text);
    ch_head = (ch_head + 1) % MAX_CHAT_MSGS;
    if (ch_count < MAX_CHAT_MSGS) ch_count++;
    ch_scroll = ch_count;
    xSemaphoreGive(ch_mutex);
}

// DM history is written per peer via history_append_dm; no more global chat.bin.
static void history_append_channel(const char *text, bool is_mine) {
    history_append_impl(CH_HISTORY_LOG_PATH, text, is_mine);
}
static void history_load_channel(void) {
    history_load_impl(CH_HISTORY_LOG_PATH, ch_ring_add_from_disk);
}

// Path for per-contact DM history: /sd/meshcore/dm/<pubkey-hex16>.bin
// 8-byte pubkey prefix as hex = 16 chars, comfortably unique for max 16 contacts.
static void history_dm_path(const uint8_t pub[32], char *out, size_t cap) {
    snprintf(out, cap, "%s/%02x%02x%02x%02x%02x%02x%02x%02x.bin",
             HISTORY_DM_DIR,
             pub[0], pub[1], pub[2], pub[3], pub[4], pub[5], pub[6], pub[7]);
}

static void history_append_dm(const uint8_t peer_pub[32], const char *text, bool is_mine) {
    if (!history_ready || peer_pub == NULL) return;
    char path[64];
    history_dm_path(peer_pub, path, sizeof(path));
    history_append_impl(path, text, is_mine);
}

// Clear the DM ring and load a specific peer's history.
// Called whenever dm_target changes.
static void history_load_dm(const uint8_t peer_pub[32]) {
    if (xSemaphoreTake(chat_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        memset(chat_msgs, 0, sizeof(chat_msgs));
        chat_head   = 0;
        chat_count  = 0;
        chat_scroll = 0;
        xSemaphoreGive(chat_mutex);
    }
    if (!history_ready || peer_pub == NULL) return;
    char path[64];
    history_dm_path(peer_pub, path, sizeof(path));
    history_load_impl(path, chat_ring_add_from_disk);
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
    uint16_t advint = 0;
    if (nvs_get_u16(handle, NVS_LORA_ADVERT_INT, &advint) == ESP_OK && advint != 0) advert_interval_s = advint;
    uint8_t role = 0;
    if (nvs_get_u8(handle, NVS_LORA_ROLE, &role) == ESP_OK && role <= MESHCORE_DEVICE_ROLE_SENSOR) {
        lora_role = (meshcore_device_role_t)role;
    }
    uint8_t phs = 0;
    if (nvs_get_u8(handle, NVS_LORA_PATHHASH, &phs) == ESP_OK && phs >= 1 && phs <= 3) {
        path_hash_size = phs;
    }
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
    nvs_set_u16(handle, NVS_LORA_ADVERT_INT, advert_interval_s);
    nvs_set_u8 (handle, NVS_LORA_ROLE, (uint8_t)lora_role);
    nvs_set_u8 (handle, NVS_LORA_PATHHASH, path_hash_size);
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
    advert.role      = lora_role;

    const char *adv_src = lora_advert_name[0] ? lora_advert_name :
                          ((owner_name[0] && owner_name[0] != '(') ? owner_name : NULL);
    if (adv_src) {
        strncpy(advert.name, adv_src, MESHCORE_MAX_NAME_SIZE);
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

static void update_node(const meshcore_advert_t* advert, uint32_t now_ms, const lora_packet_stats_t* stats) {
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

        if (stats != NULL && stats->valid) {
            int rssi_dbm = -(int)stats->rssi_pkt_raw / 2;
            if (rssi_dbm < -127) rssi_dbm = -127;
            n->last_rssi_dbm  = (int8_t)rssi_dbm;
            n->last_snr_db_x4 = stats->snr_pkt_raw;
            n->stats_valid    = true;
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

// RAM-only: for system/error messages without peer context.
// NOT persisted (would otherwise pollute every peer's chat).
static void chat_add_message(const char* text, bool is_mine) {
    if (xSemaphoreTake(chat_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    chat_msg_t* m    = &chat_msgs[chat_head];
    m->active        = true;
    m->is_mine       = is_mine;
    m->timestamp_ms  = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    utf8_sanitize(m->text, MAX_MSG_TEXT, text);
    chat_head = (chat_head + 1) % MAX_CHAT_MSGS;
    if (chat_count < MAX_CHAT_MSGS) chat_count++;
    chat_scroll = chat_count;
    xSemaphoreGive(chat_mutex);
}

// DM with peer context: always persist to peer's file; only add to ring if that
// peer is currently active (so you don't lose a DM from another peer while
// viewing peer A's chat).
static void chat_add_dm(const char* text, bool is_mine, const uint8_t peer_pub[32]) {
    if (peer_pub) history_append_dm(peer_pub, text, is_mine);
    if (dm_target_set && peer_pub &&
        memcmp(peer_pub, dm_target_pub, MESHCORE_PUB_KEY_SIZE) == 0) {
        chat_add_message(text, is_mine);
    }
}

// Switch the active DM peer + reload its history from SD.
static void dm_select_target(const uint8_t pub[32], const char *name) {
    dm_target_set = true;
    memcpy(dm_target_pub, pub, MESHCORE_PUB_KEY_SIZE);
    strncpy(dm_target_name, name ? name : "", sizeof(dm_target_name) - 1);
    dm_target_name[sizeof(dm_target_name) - 1] = '\0';
    history_load_dm(pub);
}

static void ch_add_message(const char* text, bool is_mine) {
    if (xSemaphoreTake(ch_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    chat_msg_t* m    = &ch_msgs[ch_head];
    m->active        = true;
    m->is_mine       = is_mine;
    m->timestamp_ms  = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    utf8_sanitize(m->text, MAX_MSG_TEXT, text);
    ch_head = (ch_head + 1) % MAX_CHAT_MSGS;
    if (ch_count < MAX_CHAT_MSGS) ch_count++;
    ch_scroll = ch_count;
    xSemaphoreGive(ch_mutex);

    history_append_channel(m->text, is_mine);
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
    // Include name prefix: "name: text" — same fallback as send_advert.
    const char *name_src = lora_advert_name[0] ? lora_advert_name :
                           ((owner_name[0] && owner_name[0] != '(') ? owner_name : NULL);
    char prefixed[MAX_MSG_TEXT];
    if (name_src) {
        snprintf(prefixed, sizeof(prefixed), "%s: %s", name_src, text);
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
static void advert_task(void *arg) {
    // Initial delay so the radio is fully up before first advert
    vTaskDelay(pdMS_TO_TICKS(5000));
    while (1) {
        send_advert();
        uint32_t ms = (uint32_t)advert_interval_s * 1000u;
        if (ms < 5000u) ms = 5000u;
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
}

// ── RX dedup (drop flood retransmits) ─────────────────────────────────────────
// meshcore-c has no dedup; flood-routed packets arrive multiple times via
// different repeaters. Headers/transport_codes vary between retransmits but the
// inner payload (MAC + ciphertext for DM/GRP_TXT, advert body for ADVERT) is
// identical. Fingerprint on first 16 bytes of mc_msg.payload.
#define RX_DEDUP_SIZE 32
static uint8_t rx_dedup_fp[RX_DEDUP_SIZE][16];
static int     rx_dedup_head  = 0;
static int     rx_dedup_count = 0;

static bool rx_is_duplicate(const uint8_t *payload, uint16_t payload_len) {
    uint8_t fp[16] = {0};
    int fp_len = payload_len < 16 ? payload_len : 16;
    memcpy(fp, payload, fp_len);
    for (int i = 0; i < rx_dedup_count; i++) {
        if (memcmp(rx_dedup_fp[i], fp, 16) == 0) return true;
    }
    memcpy(rx_dedup_fp[rx_dedup_head], fp, 16);
    rx_dedup_head = (rx_dedup_head + 1) % RX_DEDUP_SIZE;
    if (rx_dedup_count < RX_DEDUP_SIZE) rx_dedup_count++;
    return false;
}

// ── Noise floor poll task ─────────────────────────────────────────────────────
static void noise_floor_task(void *arg) {
    ESP_LOGI(TAG, "Noise floor task started");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));  // 60s — 5s veroorzaakte RX-decode-fail (zie diagnose 2026-05-23)
        if (!noise_floor_supported) continue;
        uint8_t   raw = 0;
        esp_err_t r   = lora_get_rssi_inst(&raw);
        if (r == ESP_OK) {
            int dbm = -(int)raw / 2;
            if (dbm < -127) dbm = -127;
            noise_floor_dbm   = (int8_t)dbm;
            noise_floor_valid = true;
        } else if (r == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "C6 firmware lacks RSSI_INST type — disabling noise floor poll");
            noise_floor_supported = false;
        }
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

            if (pkt.stats.valid) {
                int rssi_dbm        = -(int)pkt.stats.rssi_pkt_raw / 2;
                int signal_rssi_dbm = -(int)pkt.stats.signal_rssi_pkt_raw / 2;
                if (rssi_dbm < -127)        rssi_dbm = -127;
                if (signal_rssi_dbm < -127) signal_rssi_dbm = -127;
                last_rx_rssi_dbm        = (int8_t)rssi_dbm;
                last_rx_snr_db_x4       = pkt.stats.snr_pkt_raw;
                last_rx_signal_rssi_dbm = (int8_t)signal_rssi_dbm;
                last_rx_stats_ms        = now_ms;
                last_rx_stats_valid     = true;
            }

            ESP_LOGI(TAG, "RX %d bytes: %02X %02X %02X %02X (stats: %s rssi=%d snr=%d/4)",
                     pkt.length,
                     pkt.length > 0 ? pkt.data[0] : 0,
                     pkt.length > 1 ? pkt.data[1] : 0,
                     pkt.length > 2 ? pkt.data[2] : 0,
                     pkt.length > 3 ? pkt.data[3] : 0,
                     pkt.stats.valid ? "y" : "n",
                     pkt.stats.valid ? -(int)pkt.stats.rssi_pkt_raw / 2 : 0,
                     pkt.stats.valid ? (int)pkt.stats.snr_pkt_raw : 0);

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
                if (rx_is_duplicate(mc_msg.payload, mc_msg.payload_length)) {
                    ESP_LOGI(TAG, "Dedup: drop flood retransmit (type=%d)", mc_msg.type);
                    continue;
                }
                if (mc_msg.type == MESHCORE_PAYLOAD_TYPE_ADVERT) {
                    meshcore_advert_t advert;
                    if (meshcore_advert_deserialize(mc_msg.payload, mc_msg.payload_length, &advert) >= 0) {
                        update_node(&advert, now_ms, pkt.stats.valid ? &pkt.stats : NULL);
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
                        meshcore_device_role_t sender_role = MESHCORE_DEVICE_ROLE_CHAT_NODE;
                        bool    sender_found = false;
                        if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                            for (int ni = 0; ni < MAX_NODES; ni++) {
                                if (node_list[ni].active && node_list[ni].pub_key[0] == src_hash) {
                                    memcpy(sender_pub, node_list[ni].pub_key, 32);
                                    strncpy(sender_name, node_list[ni].name, sizeof(sender_name) - 1);
                                    sender_role = node_list[ni].role;
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
                        chat_add_dm(display, false, sender_pub);
                        contact_ensure(sender_pub, sender_name, (uint8_t)sender_role);
                        bool viewing_sender = (current_view == VIEW_CHAT && !dm_inbox_mode &&
                                               dm_target_set &&
                                               memcmp(sender_pub, dm_target_pub, MESHCORE_PUB_KEY_SIZE) == 0);
                        if (!viewing_sender) {
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

                            // 2. Build and encrypt inner data.
                            //    Layout: path_len_byte | type | ack_crc[4] | path_bytes[hops*bph] | zero-pad
                            //    path_len_byte encodes bytes-per-hop in upper 2 bits, hop count in lower 6.
                            //    Total size rounded up to a 16-byte AES block boundary.
                            //    We reverse the incoming hops so the receiver learns the correct
                            //    sequence of repeaters back to us. Cap so the inner buffer stays
                            //    within 2 AES blocks (32B); longer paths fall back to 0 hops.
                            uint8_t ret_bph        = mc_msg.bytes_per_hop ? mc_msg.bytes_per_hop : 1;
                            uint8_t ret_hop_count  = (ret_bph > 0) ? (mc_msg.path_length / ret_bph) : 0;
                            uint8_t ret_path_bytes = ret_hop_count * ret_bph;
                            if (ret_path_bytes > 26) {
                                ret_hop_count  = 0;
                                ret_path_bytes = 0;
                            }

                            size_t  inner_size = ((6 + (size_t)ret_path_bytes + 15) / 16) * 16;
                            if (inner_size < 16) inner_size = 16;
                            if (inner_size > 32) inner_size = 32;

                            uint8_t inner[32]       = {0};
                            uint8_t path_cipher[32] = {0};
                            inner[0] = ((uint8_t)((ret_bph - 1) & 0x03) << 6) | (ret_hop_count & 0x3F);
                            inner[1] = MESHCORE_PAYLOAD_TYPE_ACK;   // = 0x03
                            inner[2] = sha_out[0];
                            inner[3] = sha_out[1];
                            inner[4] = sha_out[2];
                            inner[5] = sha_out[3];
                            // Reverse hop-by-hop (each hop is bph bytes wide).
                            for (uint8_t h = 0; h < ret_hop_count; h++) {
                                uint8_t src_hop = ret_hop_count - 1 - h;
                                memcpy(&inner[6 + h * ret_bph],
                                       &mc_msg.path[src_hop * ret_bph],
                                       ret_bph);
                            }
                            // remaining bytes = 0 padding

                            {
                                mbedtls_aes_context aes;
                                mbedtls_aes_init(&aes);
                                mbedtls_aes_setkey_enc(&aes, good_secret, 128);
                                for (size_t bi = 0; bi < inner_size; bi += 16) {
                                    mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT,
                                                          &inner[bi], &path_cipher[bi]);
                                }
                                mbedtls_aes_free(&aes);
                            }

                            // 3. MAC = HMAC-SHA256(good_secret[32], ciphertext)[0:2]
                            uint8_t path_mac[32];
                            mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                                            good_secret, 32, path_cipher, inner_size, path_mac);

                            // 4. Build PATH packet (outer still floods)
                            meshcore_message_t path_msg = {0};
                            path_msg.type          = MESHCORE_PAYLOAD_TYPE_PATH;
                            path_msg.route         = MESHCORE_ROUTE_TYPE_FLOOD;
                            path_msg.version       = 0;
                            path_msg.path_length   = 0;
                            path_msg.payload[0]    = src_hash;        // dest hash
                            path_msg.payload[1]    = node_pub_key[0]; // src  hash (us)
                            path_msg.payload[2]    = path_mac[0];
                            path_msg.payload[3]    = path_mac[1];
                            memcpy(&path_msg.payload[4], path_cipher, inner_size);
                            path_msg.payload_length = 4 + inner_size;

                            ESP_LOGI(TAG, "PATH_RETURN: hops=%u bph=%u inner_size=%u",
                                     (unsigned)ret_hop_count, (unsigned)ret_bph,
                                     (unsigned)inner_size);

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
        case FIELD_ADVERT_INT: {
            static const uint16_t presets[] = {30, 60, 300, 900};
            const int n = (int)(sizeof(presets) / sizeof(presets[0]));
            int idx = 2;  // default to 5 min
            for (int i = 0; i < n; i++) if (presets[i] == advert_interval_s) { idx = i; break; }
            idx = ((idx + delta) % n + n) % n;
            advert_interval_s = presets[idx];
            break;
        }
        case FIELD_PRESET: {
            int idx = lora_preset_match();
            if (idx < 0) {
                // Currently custom — apply first preset on +delta, last on -delta
                idx = (delta > 0) ? 0 : (LORA_PRESET_COUNT - 1);
            } else {
                idx = ((idx + delta) % LORA_PRESET_COUNT + LORA_PRESET_COUNT) % LORA_PRESET_COUNT;
            }
            lora_cfg.spreading_factor = LORA_PRESETS[idx].sf;
            lora_cfg.bandwidth        = LORA_PRESETS[idx].bw;
            lora_cfg.coding_rate      = LORA_PRESETS[idx].cr;
            break;
        }
        case FIELD_ROLE: {
            static const meshcore_device_role_t ROLES[] = {
                MESHCORE_DEVICE_ROLE_CHAT_NODE,
                MESHCORE_DEVICE_ROLE_REPEATER,
                MESHCORE_DEVICE_ROLE_ROOM_SERVER,
                MESHCORE_DEVICE_ROLE_SENSOR,
            };
            const int n = (int)(sizeof(ROLES) / sizeof(ROLES[0]));
            int idx = 0;
            for (int i = 0; i < n; i++) if (ROLES[i] == lora_role) { idx = i; break; }
            idx = ((idx + delta) % n + n) % n;
            lora_role = ROLES[idx];
            break;
        }
        case FIELD_PATH_HASH_SIZE: {
            int v = (int)path_hash_size + delta;
            if (v < 1) v = 3;
            if (v > 3) v = 1;
            path_hash_size = (uint8_t)v;
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
    int tab_w = (w - 200) / VIEW_COUNT;  // reserve 200px right for status indicators

    pax_simple_rect(&fb, COL_HEADER, 0, 0, w, TAB_BAR_H);

    int label_y = (TAB_BAR_H - TXT_TAB) / 2;
    for (int i = 0; i < VIEW_COUNT; i++) {
        bool active = (i == (int)current_view);
        if (active) {
            pax_simple_rect(&fb, COL_ACCENT, i * tab_w, 0, tab_w, TAB_BAR_H);
        }
        pax_col_t col = active ? COL_HEADER : COL_GRAY;
        pax_vec2f sz  = pax_text_size(FONT, TXT_TAB, tab_labels[i]);
        int       tx  = i * tab_w + (tab_w - (int)sz.x) / 2;
        pax_draw_text(&fb, col, FONT, TXT_TAB, tx, label_y, tab_labels[i]);
    }
    // Underline whole bar
    pax_simple_rect(&fb, COL_PANEL, 0, TAB_BAR_H - 1, w, 1);

    // Right side: battery % + RX count — bump to TXT_BODY for readability
    char status_right[32] = {0};
    int  status_x         = w - 10;
    int  status_y         = (TAB_BAR_H - TXT_BODY) / 2;

    bsp_power_battery_information_t bat = {0};
    if (bsp_power_get_battery_information(&bat) == ESP_OK && bat.battery_available) {
        int pct = (int)bat.remaining_percentage;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        const char *chr = bat.battery_charging ? "+" : "";
        snprintf(status_right, sizeof(status_right), "%d%%%s", pct, chr);
        pax_col_t bat_col = pct <= 20 ? COL_RED : (pct <= 50 ? COL_AMBER : COL_GREEN);
        pax_vec2f sz = pax_text_size(FONT, TXT_BODY, status_right);
        status_x -= (int)sz.x;
        pax_draw_text(&fb, bat_col, FONT, TXT_BODY, status_x, status_y, status_right);
        status_x -= 14;
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_BODY, status_x, status_y, "|");
        status_x -= 14;
    }

    if (lora_rx_ok) {
        int cnt = 0;
        if (xSemaphoreTake(rx_mutex, 0) == pdTRUE) {
            cnt = rx_count;
            xSemaphoreGive(rx_mutex);
        }
        char rxbuf[12];
        snprintf(rxbuf, sizeof(rxbuf), "RX:%d", cnt);
        pax_vec2f sz = pax_text_size(FONT, TXT_BODY, rxbuf);
        status_x -= (int)sz.x;
        pax_draw_text(&fb, COL_GREEN, FONT, TXT_BODY, status_x, status_y, rxbuf);
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
    pax_background(&fb, COL_BG);
    pax_draw_text(&fb, COL_WHITE, FONT, TXT_TITLE, 14, 14, "Radio Firmware Update");
    pax_draw_text(&fb, COL_GRAY,  FONT, TXT_BODY,  14, 14 + TXT_TITLE + 8, "Stopping ESP-Hosted...");
    blit();
    radio_bootloader_mode = true;
    start_radio_bootloader();
}

// ── Render: bootloader screen ─────────────────────────────────────────────────
static void render_bootloader(void) {
    int w = (int)pax_buf_get_width(&fb);
    int h = (int)pax_buf_get_height(&fb);
    pax_background(&fb, COL_BG);
    pax_simple_rect(&fb, COL_ACCENT, 0, 0, w, TAB_BAR_H);
    pax_draw_text(&fb, COL_HEADER, FONT, TXT_TAB, 14, (TAB_BAR_H - TXT_TAB) / 2, "Radio Bootloader Mode");
    int y = TAB_BAR_H + 12;
    pax_draw_text(&fb, COL_GREEN, FONT, TXT_BODY, 14, y, "C6 is in bootloader mode."); y += TXT_BODY + 10;
    pax_draw_text(&fb, COL_WHITE, FONT, TXT_BODY, 14, y, "On your Mac:");               y += TXT_BODY + 6;
    pax_draw_text(&fb, COL_AMBER, pax_font_sky_mono, 14, 14, y, "ls /dev/cu.usbmodem*"); y += 20;
    pax_draw_text(&fb, COL_GRAY,  FONT,             TXT_SMALL, 14, y, "(find the new C6 USB device)"); y += TXT_SMALL + 8;
    pax_draw_text(&fb, COL_WHITE, pax_font_sky_mono, 14, 14, y, "cd tanmatsu-radio");                  y += 20;
    pax_draw_text(&fb, COL_AMBER, pax_font_sky_mono, 13, 14, y, "esptool.py --chip esp32c6");          y += 18;
    pax_draw_text(&fb, COL_AMBER, pax_font_sky_mono, 13, 14, y, "  --port /dev/cu.usbmodem21401");     y += 18;
    pax_draw_text(&fb, COL_AMBER, pax_font_sky_mono, 13, 14, y, "  --before no_reset write_flash");    y += 18;
    pax_draw_text(&fb, COL_AMBER, pax_font_sky_mono, 13, 14, y, "  --flash_mode dio --flash_freq 80m"); y += 18;
    pax_draw_text(&fb, COL_AMBER, pax_font_sky_mono, 13, 14, y, "  --flash_size 8MB");                 y += 18;
    pax_draw_text(&fb, COL_AMBER, pax_font_sky_mono, 13, 14, y, "  0x0 bootloader.bin 0x8000 pt.bin"); y += 18;
    pax_draw_text(&fb, COL_AMBER, pax_font_sky_mono, 13, 14, y, "  0xd000 ota.bin 0x10000 app.bin");
    int fy = h - FOOTER_H;
    pax_simple_rect(&fb, COL_HEADER, 0, fy, w, FOOTER_H);
    pax_simple_rect(&fb, COL_PANEL,  0, fy, w, 1);
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 12, fy + (FOOTER_H - TXT_SMALL) / 2,
                  "ESC / F1: restart badge after flashing");
    blit();
}

// ── Render: settings screen ───────────────────────────────────────────────────
static void render_settings(void) {
    int w = (int)pax_buf_get_width(&fb);
    int h = (int)pax_buf_get_height(&fb);

    pax_background(&fb, COL_BLACK);
    render_tab_bar();

    // Edit/dirty indicator in tab bar right area
    if (edit_mode) {
        const char *mode_str = "[EDIT]";
        pax_vec2f sz = pax_text_size(FONT, TXT_SMALL, mode_str);
        pax_draw_text(&fb, COL_AMBER, FONT, TXT_SMALL, w - (int)sz.x - 110,
                      (TAB_BAR_H - TXT_SMALL) / 2, mode_str);
    }

    typedef struct { const char *label; char value[64]; } row_t;
    row_t rows[FIELD_COUNT];

    snprintf(rows[FIELD_OWNER].value, sizeof(rows[FIELD_OWNER].value), "%s", owner_name);
    rows[FIELD_OWNER].label = "Owner name";

    rows[FIELD_ADV_NAME].label = "Advert name";
    snprintf(rows[FIELD_ADV_NAME].value, sizeof(rows[FIELD_ADV_NAME].value), "%s",
             lora_advert_name[0] ? lora_advert_name : "(use owner)");

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

    rows[FIELD_ADVERT_INT].label = "Advert interval";
    if (advert_interval_s < 60) {
        snprintf(rows[FIELD_ADVERT_INT].value, sizeof(rows[FIELD_ADVERT_INT].value), "%us", (unsigned)advert_interval_s);
    } else {
        snprintf(rows[FIELD_ADVERT_INT].value, sizeof(rows[FIELD_ADVERT_INT].value), "%umin", (unsigned)(advert_interval_s / 60));
    }

    rows[FIELD_PRESET].label = "LoRa preset";
    {
        int pidx = lora_preset_match();
        if (pidx >= 0) {
            snprintf(rows[FIELD_PRESET].value, sizeof(rows[FIELD_PRESET].value), "%s", LORA_PRESETS[pidx].name);
        } else {
            snprintf(rows[FIELD_PRESET].value, sizeof(rows[FIELD_PRESET].value), "(custom)");
        }
    }

    rows[FIELD_ROLE].label = "Role";
    snprintf(rows[FIELD_ROLE].value, sizeof(rows[FIELD_ROLE].value), "%s", role_label(lora_role));

    rows[FIELD_PATH_HASH_SIZE].label = "Path hash size";
    {
        static const char *hops[] = {"64 hops", "32 hops", "21 hops"};
        int hi = (path_hash_size >= 1 && path_hash_size <= 3) ? (path_hash_size - 1) : 0;
        snprintf(rows[FIELD_PATH_HASH_SIZE].value, sizeof(rows[FIELD_PATH_HASH_SIZE].value),
                 "%u byte (%s)", path_hash_size, hops[hi]);
    }

    rows[FIELD_REGION_SCOPE].label = "Region scope";
    snprintf(rows[FIELD_REGION_SCOPE].value, sizeof(rows[FIELD_REGION_SCOPE].value),
             "%s", region_scope[0] ? region_scope : "(not set)");

    const int row_h        = 44;
    const int footer_h     = 60;
    const int y0           = TAB_BAR_H + 6;
    const int list_h       = h - y0 - footer_h;
    int rows_vis           = list_h / row_h;
    if (rows_vis < 1)            rows_vis = 1;
    if (rows_vis > FIELD_COUNT)  rows_vis = FIELD_COUNT;

    if (selected < settings_scroll)             settings_scroll = selected;
    if (selected >= settings_scroll + rows_vis) settings_scroll = selected - rows_vis + 1;
    int max_scroll = FIELD_COUNT - rows_vis;
    if (max_scroll < 0)              max_scroll = 0;
    if (settings_scroll > max_scroll) settings_scroll = max_scroll;
    if (settings_scroll < 0)         settings_scroll = 0;

    int text_y_off = (row_h - TXT_BODY) / 2;

    for (int row = 0; row < rows_vis; row++) {
        int  i      = row + settings_scroll;
        int  y      = y0 + row * row_h;
        bool is_sel = (i == selected);

        if (is_sel) {
            pax_col_t bg  = edit_mode ? 0xFF3A2A1A : COL_PANEL;
            pax_col_t bar = edit_mode ? COL_AMBER  : COL_ACCENT;
            pax_simple_rect(&fb, bg,  0, y, w, row_h - 1);
            pax_simple_rect(&fb, bar, 0, y, 5, row_h - 1);
        }

        pax_simple_rect(&fb, COL_PANEL, 12, y + row_h - 1, w - 24, 1);

        pax_col_t lbl_col = is_sel ? COL_WHITE : COL_GRAY;
        pax_draw_text(&fb, lbl_col, FONT, TXT_BODY, 18, y + text_y_off, rows[i].label);

        pax_col_t val_col;
        if (i >= FIELD_FREQ && !c6_available) {
            val_col = COL_AMBER;
        } else if (is_sel && edit_mode) {
            val_col = COL_AMBER;
        } else if (is_sel) {
            val_col = COL_WHITE;
        } else {
            val_col = COL_GREEN;
        }

        char val_disp[80];
        bool is_text_field = (i == FIELD_OWNER || i == FIELD_ADV_NAME || i == FIELD_REGION_SCOPE);
        if (is_sel && edit_mode && field_editing_text && is_text_field) {
            snprintf(val_disp, sizeof(val_disp), "%s_", field_edit_buf);
        } else if (is_sel && edit_mode && !is_text_field) {
            snprintf(val_disp, sizeof(val_disp), "< %s >", rows[i].value);
        } else {
            snprintf(val_disp, sizeof(val_disp), "%s", rows[i].value);
        }
        pax_vec2f vsz = pax_text_size(FONT, TXT_BODY, val_disp);
        pax_draw_text(&fb, val_col, FONT, TXT_BODY, w - (int)vsz.x - 18, y + text_y_off, val_disp);
    }

    if (FIELD_COUNT > rows_vis) {
        char sc[40];
        snprintf(sc, sizeof(sc), "%d-%d/%d", settings_scroll + 1, settings_scroll + rows_vis, FIELD_COUNT);
        pax_vec2f sz = pax_text_size(FONT, TXT_BODY, sc);
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_BODY, w - (int)sz.x - 10, h - footer_h - TXT_BODY - 2, sc);
    }

    int fy = h - footer_h;
    pax_simple_rect(&fb, COL_HEADER, 0, fy, w, footer_h);
    pax_simple_rect(&fb, COL_PANEL, 0, fy, w, 1);

    const char *hint = NULL;
    pax_col_t   hint_col = COL_GRAY;
    if (edit_mode && field_editing_text) {
        hint = "Type to edit   Backspace: del   Enter: save   ESC: cancel";
    } else if (edit_mode) {
        hint = "Up/Down or W/S: adjust   Enter: save   ESC: cancel";
    } else if (!c6_available) {
        hint = "NVS only — C6 unavailable   U: flash radio";
        hint_col = COL_AMBER;
    } else if (selected == FIELD_OWNER) {
        hint = "Owner name is shared with launcher (Enter to edit)";
    } else if (selected == FIELD_ADV_NAME) {
        hint = "Advert name overrides owner in LoRa adverts (empty=use owner)";
    } else if (selected == FIELD_SYNC) {
        hint = "Sync word: 0x12 = public LoRa. Isolates networks.";
    } else if (selected == FIELD_PREAMBLE) {
        hint = "Preamble: longer = detects weak signal, more airtime";
    } else if (selected == FIELD_ADVERT_INT) {
        hint = "Advert interval: longer = lower duty cycle, saves battery";
    } else if (selected == FIELD_PRESET) {
        hint = "Preset overwrites SF/BW/CR. MeshCore = default net.";
    } else if (selected == FIELD_ROLE) {
        hint = "Role: advertised only. Does NOT enable repeater behavior.";
    } else {
        hint = "W/S: navigate   Enter: edit   R: reload   Tab: next   U: flash";
    }
    pax_draw_text(&fb, hint_col, FONT, TXT_BODY, 10, fy + 6, hint);

    if (dirty) {
        const char *unsaved = "* unsaved";
        pax_vec2f usz = pax_text_size(FONT, TXT_BODY, unsaved);
        pax_draw_text(&fb, COL_AMBER, FONT, TXT_BODY, w - (int)usz.x - 10, fy + 6, unsaved);
    }

    // SNTP / time status line (left) + RF stats (right) on second footer row
    int row2_y = fy + 6 + TXT_BODY + 6;
    {
        time_t    now = time(NULL);
        struct tm t;
        localtime_r(&now, &t);
        char ts[48];
        pax_col_t ts_col;
        if (sntp_synced) {
            snprintf(ts, sizeof(ts), "SNTP %02d:%02d:%02d  %02d-%02d-%04d",
                     t.tm_hour, t.tm_min, t.tm_sec,
                     t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
            ts_col = COL_GREEN;
        } else if (time_from_nvs) {
            snprintf(ts, sizeof(ts), "~%02d:%02d %02d-%02d (NVS, approx)",
                     t.tm_hour, t.tm_min, t.tm_mday, t.tm_mon + 1);
            ts_col = COL_AMBER;
        } else {
            snprintf(ts, sizeof(ts), "no time sync — timestamps incorrect");
            ts_col = COL_RED;
        }
        pax_draw_text(&fb, ts_col, FONT, TXT_BODY, 10, row2_y, ts);
    }
    {
        char rf[64];
        int  snr_dB = (int)last_rx_snr_db_x4 / 4;
        if (last_rx_stats_valid && noise_floor_valid) {
            snprintf(rf, sizeof(rf), "RX:%d SNR:%+d N:%d",
                     (int)last_rx_rssi_dbm, snr_dB, (int)noise_floor_dbm);
        } else if (last_rx_stats_valid) {
            snprintf(rf, sizeof(rf), "RX:%d SNR:%+d", (int)last_rx_rssi_dbm, snr_dB);
        } else if (noise_floor_valid) {
            snprintf(rf, sizeof(rf), "noise:%d", (int)noise_floor_dbm);
        } else {
            rf[0] = '\0';
        }
        if (rf[0]) {
            pax_vec2f rsz = pax_text_size(FONT, TXT_BODY, rf);
            pax_draw_text(&fb, COL_GRAY, FONT, TXT_BODY, w - (int)rsz.x - 10, row2_y, rf);
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

    // Use same name as send_advert: lora_advert_name overrides owner_name when set.
    const char *adv_src = lora_advert_name[0] ? lora_advert_name :
                          ((owner_name[0] && owner_name[0] != '(') ? owner_name : "");

    // URL-encode (replace spaces with +)
    char encoded_name[64];
    int ei = 0;
    for (int i = 0; adv_src[i] && ei < 62; i++) {
        char c = adv_src[i];
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

    pax_background(&fb, COL_BG);

    if (!ok) {
        pax_draw_text(&fb, COL_AMBER, FONT, TXT_BODY, 20, h / 2, "QR encode failed");
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
    pax_vec2f lsz = pax_text_size(FONT, TXT_TITLE, label);
    pax_draw_text(&fb, COL_AMBER, FONT, TXT_TITLE,
                  (w - (int)lsz.x) / 2, qr_y - margin - TXT_TITLE - 6, label);

    // Name below QR
    char name_label[80];
    snprintf(name_label, sizeof(name_label), "%s  [press any key to close]",
             adv_src[0] ? adv_src : "(no name)");
    pax_vec2f nsz = pax_text_size(FONT, TXT_SMALL, name_label);
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL,
                  (w - (int)nsz.x) / 2, qr_y + qr_px + margin + 6, name_label);

    blit();
}

// ── Render: nodes screen ──────────────────────────────────────────────────────
#define NODES_ROW_H    44
#define NODES_Y0       (TAB_BAR_H + 4)
#define NODES_HEADER_H 26

static void render_nodes(void) {
    int w = (int)pax_buf_get_width(&fb);
    int h = (int)pax_buf_get_height(&fb);

    pax_background(&fb, COL_BG);
    render_tab_bar();

    int hdr_y = NODES_Y0;
    pax_simple_rect(&fb, COL_HEADER, 0, hdr_y, w, NODES_HEADER_H);
    pax_simple_rect(&fb, COL_PANEL,  0, hdr_y + NODES_HEADER_H - 1, w, 1);

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
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 8,            hdr_text_y, "Role");
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 96,           hdr_text_y, "Name");
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, rssi_hdr_x,   hdr_text_y, "RSSI");
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, snr_hdr_x,    hdr_text_y, "SNR");
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, pkts_hdr_x,   hdr_text_y, "#Pkt");
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, dist_hdr_x,   hdr_text_y, "Dist");
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, age_hdr_x,    hdr_text_y, "Seen");

    int list_y0   = NODES_Y0 + NODES_HEADER_H;
    int footer_h  = 60;
    int list_h    = h - footer_h - list_y0;
    int rows_vis  = list_h / NODES_ROW_H;
    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    int row_text_y = (NODES_ROW_H - TXT_BODY) / 2;

    if (!lora_rx_ok) {
        pax_draw_text(&fb, COL_AMBER, FONT, TXT_BODY, 12, list_y0 + 14, "LoRa radio not available");
    } else if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (node_count == 0 && contact_count == 0) {
            pax_draw_text(&fb, COL_GRAY, FONT, TXT_BODY, 12, list_y0 + 14, "Listening... no nodes heard yet.");
        } else {
            display_row_t rows_dl[MAX_CONTACTS + MAX_NODES];
            int idx_count = build_node_display(rows_dl, MAX_CONTACTS + MAX_NODES);

            int max_scroll = idx_count - rows_vis;
            if (max_scroll < 0) max_scroll = 0;
            if (node_scroll > max_scroll) node_scroll = max_scroll;
            if (node_scroll < 0)         node_scroll = 0;

            if (node_cursor >= idx_count) node_cursor = idx_count > 0 ? idx_count - 1 : 0;
            if (node_cursor < 0)          node_cursor = 0;
            if (node_cursor < node_scroll)              node_scroll = node_cursor;
            if (node_cursor >= node_scroll + rows_vis)  node_scroll = node_cursor - rows_vis + 1;

            for (int row = 0; row < rows_vis; row++) {
                int list_idx = row + node_scroll;
                if (list_idx >= idx_count) break;
                display_row_t *d = &rows_dl[list_idx];
                node_entry_t  *n = (d->node_idx >= 0) ? &node_list[d->node_idx] : NULL;

                int y = list_y0 + row * NODES_ROW_H;
                bool is_cursor = (list_idx == node_cursor);

                if (is_cursor) {
                    pax_simple_rect(&fb, COL_PANEL, 0, y, w, NODES_ROW_H);
                    pax_simple_rect(&fb, COL_ACCENT, 0, y, 5, NODES_ROW_H);
                }

                int age_x  = w - age_col_w  - 6;
                int dist_x = age_x - dist_col_w;
                int pkts_x = dist_x - pkts_col_w;
                int snr_x  = pkts_x - snr_col_w;
                int rssi_x = snr_x - rssi_col_w;

                char age_buf[20];
                if (n) {
                    uint32_t age_s = (now_ms - n->last_seen_ms) / 1000;
                    if (age_s < 60)        snprintf(age_buf, sizeof(age_buf), "%lus", (unsigned long)age_s);
                    else if (age_s < 3600) snprintf(age_buf, sizeof(age_buf), "%lum", (unsigned long)(age_s / 60));
                    else                   snprintf(age_buf, sizeof(age_buf), "%luh", (unsigned long)(age_s / 3600));
                } else {
                    snprintf(age_buf, sizeof(age_buf), "--");
                }
                pax_draw_text(&fb, COL_GRAY, FONT, TXT_BODY, age_x, y + row_text_y, age_buf);

                pax_draw_text(&fb, COL_GRAY, FONT, TXT_BODY, dist_x, y + row_text_y, "--");

                char pkts_buf[8];
                if (n) snprintf(pkts_buf, sizeof(pkts_buf), "#%d", n->packet_count);
                else   snprintf(pkts_buf, sizeof(pkts_buf), "--");
                pax_draw_text(&fb, COL_GRAY, FONT, TXT_BODY, pkts_x, y + row_text_y, pkts_buf);

                char rssi_buf[8], snr_buf[8];
                pax_col_t rssi_col, snr_col;
                if (n && n->stats_valid) {
                    int rssi_dbm = (int)n->last_rssi_dbm;
                    int snr_dB   = (int)n->last_snr_db_x4 / 4;
                    snprintf(rssi_buf, sizeof(rssi_buf), "%d", rssi_dbm);
                    snprintf(snr_buf,  sizeof(snr_buf),  "%+d", snr_dB);
                    rssi_col = (rssi_dbm >= -80)  ? COL_GREEN :
                               (rssi_dbm >= -105) ? COL_AMBER : COL_RED;
                    snr_col  = (snr_dB  >=  0)    ? COL_GREEN :
                               (snr_dB  >= -10)   ? COL_AMBER : COL_RED;
                } else {
                    snprintf(rssi_buf, sizeof(rssi_buf), "--");
                    snprintf(snr_buf,  sizeof(snr_buf),  "--");
                    rssi_col = COL_GRAY;
                    snr_col  = COL_GRAY;
                }
                pax_draw_text(&fb, rssi_col, FONT, TXT_BODY, rssi_x, y + row_text_y, rssi_buf);
                pax_draw_text(&fb, snr_col,  FONT, TXT_BODY, snr_x,  y + row_text_y, snr_buf);

                meshcore_device_role_t role = n ? n->role : (meshcore_device_role_t)contacts[d->contact_idx].role;
                const char *src_name = n ? n->name : contacts[d->contact_idx].alias;

                const char* rl = role_label(role);
                pax_col_t role_col = (role == MESHCORE_DEVICE_ROLE_REPEATER)    ? COL_BLUE :
                                     (role == MESHCORE_DEVICE_ROLE_ROOM_SERVER) ? 0xFFBB9AF7 :
                                     (role == MESHCORE_DEVICE_ROLE_SENSOR)      ? COL_AMBER :
                                                                                  COL_GREEN;
                pax_draw_text(&fb, role_col, FONT, TXT_BODY, 8, y + row_text_y, rl);

                int name_x = 96;
                if (d->is_contact) {
                    pax_draw_text(&fb, COL_AMBER, FONT, TXT_BODY, 78, y + row_text_y, "*");
                }

                char name_trunc[25];
                int  max_name_w = rssi_x - name_x - 6;
                int  max_chars  = max_name_w / 11;  // saira regular avg ~11px @20
                if (max_chars > 24) max_chars = 24;
                if (max_chars < 1)  max_chars = 1;
                snprintf(name_trunc, sizeof(name_trunc), "%.*s", max_chars, src_name);
                pax_col_t name_col = is_cursor ? COL_WHITE :
                                     (n == NULL ? COL_GRAY : COL_WHITE);
                pax_draw_text(&fb, name_col, FONT, TXT_BODY, name_x, y + row_text_y, name_trunc);

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

    int fy_base  = h - footer_h;
    pax_simple_rect(&fb, COL_HEADER, 0, fy_base, w, footer_h);
    pax_simple_rect(&fb, COL_PANEL,  0, fy_base, w, 1);

    // Row 1: counts + filter pill (if active) + controls.
    int fx = 10;
    int fy_text = fy_base + 6;
    char counts[48];
    snprintf(counts, sizeof(counts), "Nodes:%d  Contacts:%d", node_count, contact_count);
    pax_vec2f csz = pax_text_size(FONT, TXT_BODY, counts);
    pax_draw_text(&fb, COL_WHITE, FONT, TXT_BODY, fx, fy_text, counts);
    fx += (int)csz.x + 20;

    if (node_filter != MESHCORE_DEVICE_ROLE_UNKNOWN) {
        char pill[40];
        snprintf(pill, sizeof(pill), "filter: %s", role_label(node_filter));
        pax_vec2f psz = pax_text_size(FONT, TXT_BODY, pill);
        // Amber pill background
        pax_simple_rect(&fb, COL_AMBER, fx - 6, fy_text - 2, (int)psz.x + 12, TXT_BODY + 4);
        pax_draw_text(&fb, COL_HEADER, FONT, TXT_BODY, fx, fy_text, pill);
        fx += (int)psz.x + 22;
    }

    const char *ctrl = (node_filter == MESHCORE_DEVICE_ROLE_UNKNOWN)
        ? "W/S nav   A:advert   F:fav   L:filter   Q:QR"
        : "L:next   F:fav   A:advert";
    pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, fx, fy_text + (TXT_BODY - TXT_SMALL) / 2, ctrl);

    // Row 2: advert age
    if (identity_ready) {
        uint32_t now_ms2 = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        char adv_buf[48];
        if (last_advert_ms == 0) {
            snprintf(adv_buf, sizeof(adv_buf), "advert: pending");
        } else {
            uint32_t age_s = (now_ms2 - last_advert_ms) / 1000;
            snprintf(adv_buf, sizeof(adv_buf), "last advert: %lus ago", (unsigned long)age_s);
        }
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 10, fy_text + TXT_BODY + 6, adv_buf);
    }
    blit();
}

// ── Render: chat screen ───────────────────────────────────────────────────────
static void render_chat(void) {
    int w  = (int)pax_buf_get_width(&fb);
    int h  = (int)pax_buf_get_height(&fb);

    pax_background(&fb, COL_BG);
    render_tab_bar();

    // ── Inbox view: lijst van DM-gesprekken ───────────────────────────────────
    if (dm_inbox_mode) {
        int inbox_y0 = TAB_BAR_H + 6;
        int footer_h = 36;
        int inbox_h  = h - inbox_y0 - footer_h;
        int row_h    = 56;
        int rows_vis = inbox_h / row_h;
        if (rows_vis < 1) rows_vis = 1;

        // Build inbox list: optional active DM target on top + saved contacts.
        // entry: index -1 = active dm_target row, otherwise contact index.
        int idx_map[MAX_CONTACTS + 1];
        int idx_count = 0;
        bool active_on_top = dm_target_set;
        if (active_on_top) idx_map[idx_count++] = -1;
        if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            for (int i = 0; i < contact_count && idx_count < MAX_CONTACTS + 1; i++) {
                if (active_on_top && memcmp(contacts[i].pub_key, dm_target_pub, MESHCORE_PUB_KEY_SIZE) == 0)
                    continue;  // already shown as active row
                idx_map[idx_count++] = i;
            }
            xSemaphoreGive(node_mutex);
        }

        if (idx_count == 0) {
            pax_draw_text(&fb, COL_AMBER, FONT, TXT_BODY, 16, inbox_y0 + 18,
                          "No conversations yet");
            pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 16, inbox_y0 + 18 + TXT_BODY + 6,
                          "Open the Nodes tab and press Enter on a contact.");
        } else {
            if (dm_inbox_cursor >= idx_count) dm_inbox_cursor = idx_count - 1;
            if (dm_inbox_cursor < 0)          dm_inbox_cursor = 0;
            if (dm_inbox_cursor < dm_inbox_scroll)              dm_inbox_scroll = dm_inbox_cursor;
            if (dm_inbox_cursor >= dm_inbox_scroll + rows_vis)  dm_inbox_scroll = dm_inbox_cursor - rows_vis + 1;
            int max_scroll = idx_count - rows_vis;
            if (max_scroll < 0) max_scroll = 0;
            if (dm_inbox_scroll > max_scroll) dm_inbox_scroll = max_scroll;
            if (dm_inbox_scroll < 0)          dm_inbox_scroll = 0;

            for (int row = 0; row < rows_vis; row++) {
                int li = row + dm_inbox_scroll;
                if (li >= idx_count) break;
                int  e          = idx_map[li];
                bool is_active  = (e == -1);
                bool is_cursor  = (li == dm_inbox_cursor);
                const char *name;
                meshcore_device_role_t role;
                if (is_active) {
                    name = dm_target_name;
                    int ci = contact_find(dm_target_pub);
                    role = (ci >= 0) ? (meshcore_device_role_t)contacts[ci].role
                                     : MESHCORE_DEVICE_ROLE_CHAT_NODE;
                } else {
                    name = contacts[e].alias;
                    role = (meshcore_device_role_t)contacts[e].role;
                }

                int y = inbox_y0 + row * row_h;
                if (is_cursor) {
                    pax_simple_rect(&fb, COL_PANEL, 0, y, w, row_h - 2);
                    pax_simple_rect(&fb, COL_ACCENT, 0, y, 5, row_h - 2);
                }
                pax_simple_rect(&fb, COL_PANEL, 12, y + row_h - 1, w - 24, 1);

                // Avatar circle (initial letter on amber/blue background)
                int av_x = 18, av_y = y + (row_h - 36) / 2, av_d = 36;
                pax_col_t av_bg = is_active ? COL_AMBER : COL_BLUE;
                pax_simple_rect(&fb, av_bg, av_x, av_y, av_d, av_d);
                char init[2] = {(char)(name[0] ? toupper((unsigned char)name[0]) : '?'), 0};
                pax_vec2f isz = pax_text_size(FONT, TXT_TITLE, init);
                pax_draw_text(&fb, COL_HEADER, FONT, TXT_TITLE,
                              av_x + (av_d - (int)isz.x) / 2,
                              av_y + (av_d - TXT_TITLE) / 2 - 1, init);

                // Name (line 1)
                pax_col_t name_col = is_cursor ? COL_WHITE : COL_WHITE;
                pax_draw_text(&fb, name_col, FONT, TXT_BODY, av_x + av_d + 12, y + 6, name);

                // Subline (role + active marker)
                const char *rl = role_label(role);
                char sub[64];
                if (is_active) snprintf(sub, sizeof(sub), "%s  ·  active DM", rl);
                else           snprintf(sub, sizeof(sub), "%s  ·  saved contact", rl);
                pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL,
                              av_x + av_d + 12, y + 6 + TXT_BODY + 4, sub);

                // Right-side hint
                if (is_cursor) {
                    const char *cta = "Enter ›";
                    pax_vec2f sz = pax_text_size(FONT, TXT_SMALL, cta);
                    pax_draw_text(&fb, COL_AMBER, FONT, TXT_SMALL,
                                  w - (int)sz.x - 12, y + (row_h - TXT_SMALL) / 2, cta);
                }
            }

            if (idx_count > rows_vis) {
                char sc[24];
                snprintf(sc, sizeof(sc), "%d/%d", dm_inbox_cursor + 1, idx_count);
                pax_vec2f sz = pax_text_size(FONT, TXT_SMALL, sc);
                pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL,
                              w - (int)sz.x - 10, h - footer_h - TXT_SMALL - 2, sc);
            }
        }

        int fy_base = h - footer_h;
        pax_simple_rect(&fb, COL_HEADER, 0, fy_base, w, footer_h);
        pax_simple_rect(&fb, COL_PANEL,  0, fy_base, w, 1);
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 10, fy_base + (footer_h - TXT_SMALL) / 2,
                      "W/S: nav   Enter: open   D: delete   Tab: next");
        blit();
        return;
    }

    // ── Chat view: history van geselecteerde DM ───────────────────────────────
    int input_y = h - CHAT_INPUT_H - FOOTER_H;
    int list_y0 = CHAT_Y0 + 32;        // room for header with contact name
    int list_h  = input_y - list_y0;
    int rows_vis = list_h / CHAT_ROW_H;

    // Chat header bar
    pax_simple_rect(&fb, COL_PANEL, 0, CHAT_Y0, w, 28);
    {
        char hdr[MESHCORE_MAX_NAME_SIZE + 24];
        snprintf(hdr, sizeof(hdr), "‹  %s", dm_target_set ? dm_target_name : "(no target)");
        pax_draw_text(&fb, COL_WHITE, FONT, TXT_BODY, 10, CHAT_Y0 + 4, hdr);
    }

    if (xSemaphoreTake(chat_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (chat_count == 0) {
            pax_draw_text(&fb, COL_GRAY, FONT, TXT_BODY, 14, list_y0 + 10,
                          "No messages yet. Press T to type.");
        } else {
            int max_scroll = chat_count;
            if (chat_scroll > max_scroll) chat_scroll = max_scroll;
            if (chat_scroll < rows_vis)   chat_scroll = rows_vis;

            for (int row = 0; row < rows_vis; row++) {
                int msg_idx_in_list = chat_scroll - rows_vis + row;
                if (msg_idx_in_list < 0) continue;
                int ring_idx = (chat_head - chat_count + msg_idx_in_list + MAX_CHAT_MSGS * 2) % MAX_CHAT_MSGS;
                chat_msg_t* m = &chat_msgs[ring_idx];
                if (!m->active) continue;

                int y = list_y0 + row * CHAT_ROW_H;
                if (m->is_mine) {
                    pax_vec2f sz = pax_text_size(FONT, TXT_BODY, m->text);
                    int tx = w - (int)sz.x - 16;
                    if (tx < 16) tx = 16;
                    pax_simple_rect(&fb, COL_PANEL, tx - 6, y + 2, (int)sz.x + 12, CHAT_ROW_H - 8);
                    pax_draw_text(&fb, COL_BLUE, FONT, TXT_BODY, tx, y + 6, m->text);
                    pax_draw_text(&fb, COL_GRAY, FONT, TXT_TINY, tx, y + CHAT_ROW_H - TXT_TINY - 4, "You");
                } else {
                    pax_draw_text(&fb, COL_WHITE, FONT, TXT_BODY, 14, y + 6, m->text);
                }
            }
        }
        xSemaphoreGive(chat_mutex);
    }

    // Input bar
    int iy = input_y;
    pax_simple_rect(&fb, COL_PANEL, 0, iy, w, CHAT_INPUT_H);
    pax_simple_rect(&fb, chat_typing ? COL_ACCENT : COL_AMBER, 0, iy, w, 2);
    if (chat_typing) {
        char prefix[MESHCORE_MAX_NAME_SIZE + 8];
        snprintf(prefix, sizeof(prefix), "DM %s> ", dm_target_name);
        char disp[MAX_INPUT_LEN + sizeof(prefix) + 2];
        snprintf(disp, sizeof(disp), "%s%s_", prefix, chat_input);
        pax_draw_text(&fb, COL_WHITE, FONT, TXT_BODY, 10, iy + (CHAT_INPUT_H - TXT_BODY) / 2, disp);
        char ctr[12];
        snprintf(ctr, sizeof(ctr), "%d/%d", chat_input_len, MAX_INPUT_LEN);
        pax_vec2f csz = pax_text_size(FONT, TXT_SMALL, ctr);
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, w - (int)csz.x - 10, iy + (CHAT_INPUT_H - TXT_SMALL) / 2, ctr);
    } else {
        pax_draw_text(&fb, COL_AMBER, FONT, TXT_SMALL, 10, iy + (CHAT_INPUT_H - TXT_SMALL) / 2,
                      "T: type message");
    }

    // Footer
    int fy = h - FOOTER_H;
    pax_simple_rect(&fb, COL_HEADER, 0, fy, w, FOOTER_H);
    pax_simple_rect(&fb, COL_PANEL, 0, fy, w, 1);
    if (chat_typing) {
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 10, fy + (FOOTER_H - TXT_SMALL) / 2,
                      "Enter: send   ESC: cancel   Backspace: delete");
    } else {
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 10, fy + (FOOTER_H - TXT_SMALL) / 2,
                      "T: type   W/S: scroll   ESC: back to inbox   Tab: next tab");
    }
    blit();
}

// ── Render: channel screen ────────────────────────────────────────────────────
static void render_channel(void) {
    int w  = (int)pax_buf_get_width(&fb);
    int h  = (int)pax_buf_get_height(&fb);

    pax_background(&fb, COL_BG);
    render_tab_bar();

    // Header bar
    pax_simple_rect(&fb, COL_PANEL, 0, CHAT_Y0, w, 28);
    pax_draw_text(&fb, COL_WHITE, FONT, TXT_BODY, 10, CHAT_Y0 + 4, "Public channel");

    int input_y = h - CHAT_INPUT_H - FOOTER_H;
    int list_y0 = CHAT_Y0 + 32;
    int list_h  = input_y - list_y0;
    int rows_vis = list_h / CHAT_ROW_H;

    if (xSemaphoreTake(ch_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (ch_count == 0) {
            pax_draw_text(&fb, COL_GRAY, FONT, TXT_BODY, 14, list_y0 + 10,
                          "No channel messages yet. Press T to send.");
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

                int y = list_y0 + row * CHAT_ROW_H;
                if (m->is_mine) {
                    pax_vec2f sz = pax_text_size(FONT, TXT_BODY, m->text);
                    int tx = w - (int)sz.x - 16;
                    if (tx < 16) tx = 16;
                    pax_simple_rect(&fb, COL_PANEL, tx - 6, y + 2, (int)sz.x + 12, CHAT_ROW_H - 8);
                    pax_draw_text(&fb, COL_BLUE, FONT, TXT_BODY, tx, y + 6, m->text);
                    pax_draw_text(&fb, COL_GRAY, FONT, TXT_TINY, tx, y + CHAT_ROW_H - TXT_TINY - 4, "You");
                } else {
                    pax_draw_text(&fb, COL_WHITE, FONT, TXT_BODY, 14, y + 6, m->text);
                }
            }
        }
        xSemaphoreGive(ch_mutex);
    }

    // Input bar
    int iy = input_y;
    pax_simple_rect(&fb, COL_PANEL, 0, iy, w, CHAT_INPUT_H);
    pax_simple_rect(&fb, chat_typing ? COL_ACCENT : COL_GREEN, 0, iy, w, 2);
    if (chat_typing) {
        char disp[MAX_INPUT_LEN + 4];
        snprintf(disp, sizeof(disp), "> %s_", chat_input);
        pax_draw_text(&fb, COL_WHITE, FONT, TXT_BODY, 10, iy + (CHAT_INPUT_H - TXT_BODY) / 2, disp);
        char ctr[12];
        snprintf(ctr, sizeof(ctr), "%d/%d", chat_input_len, MAX_INPUT_LEN);
        pax_vec2f csz = pax_text_size(FONT, TXT_SMALL, ctr);
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, w - (int)csz.x - 10, iy + (CHAT_INPUT_H - TXT_SMALL) / 2, ctr);
    } else {
        pax_draw_text(&fb, COL_GREEN, FONT, TXT_SMALL, 10, iy + (CHAT_INPUT_H - TXT_SMALL) / 2,
                      "T: send channel message");
    }

    int fy = h - FOOTER_H;
    pax_simple_rect(&fb, COL_HEADER, 0, fy, w, FOOTER_H);
    pax_simple_rect(&fb, COL_PANEL, 0, fy, w, 1);
    if (chat_typing) {
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 10, fy + (FOOTER_H - TXT_SMALL) / 2,
                      "Enter: send   ESC: cancel   Backspace: delete");
    } else {
        pax_draw_text(&fb, COL_GRAY, FONT, TXT_SMALL, 10, fy + (FOOTER_H - TXT_SMALL) / 2,
                      "T: type   W/S: scroll   R: clear history   Tab: next");
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
// Settings: text-field edit helpers (shared between handle_nav and handle_key).
static void settings_begin_text_edit(field_t f) {
    const char *src = "";
    if (f == FIELD_OWNER && owner_name[0] && owner_name[0] != '(') src = owner_name;
    else if (f == FIELD_ADV_NAME && lora_advert_name[0])           src = lora_advert_name;
    else if (f == FIELD_REGION_SCOPE && region_scope[0])           src = region_scope;
    strncpy(field_edit_buf, src, sizeof(field_edit_buf) - 1);
    field_edit_buf[sizeof(field_edit_buf) - 1] = '\0';
    field_edit_len     = (int)strlen(field_edit_buf);
    field_editing_text = true;
}
static void settings_commit_text_edit(field_t f) {
    if (f == FIELD_OWNER) {
        strncpy(owner_name, field_edit_buf, sizeof(owner_name) - 1);
        owner_name[sizeof(owner_name) - 1] = '\0';
        save_owner_name();
    } else if (f == FIELD_ADV_NAME) {
        strncpy(lora_advert_name, field_edit_buf, sizeof(lora_advert_name) - 1);
        lora_advert_name[sizeof(lora_advert_name) - 1] = '\0';
        save_lora_advert_name();
    } else if (f == FIELD_REGION_SCOPE) {
        strncpy(region_scope, field_edit_buf, sizeof(region_scope) - 1);
        region_scope[sizeof(region_scope) - 1] = '\0';
        save_region_scope();
    }
    field_editing_text = false;
}

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
            edit_mode          = false;
            field_editing_text = false;
            dirty              = false;
            load_owner_name();
            load_lora_advert_name();
            load_region_scope();
            load_lora_config();
        } else if (chat_typing) {
            // ESC during typing is cancelled by handle_key; don't fall through to launcher restart
        } else if (current_view == VIEW_CHAT && !dm_inbox_mode) {
            // Back to inbox instead of clearing target (keeps dm_target for return)
            dm_inbox_mode = true;
        } else {
            bsp_led_set_mode(true);
            bsp_device_restart_to_launcher();
        }
    } else if (key == BSP_INPUT_NAVIGATION_KEY_UP) {
        if (current_view == VIEW_SETTINGS) {
            if (!edit_mode) selected = (selected - 1 + FIELD_COUNT) % FIELD_COUNT;
            else if (!field_editing_text) field_adjust(selected, +1);
        } else if (current_view == VIEW_NODES) {
            if (node_cursor > 0) node_cursor--;
        } else if (current_view == VIEW_CHAT && dm_inbox_mode) {
            if (dm_inbox_cursor > 0) dm_inbox_cursor--;
        }
    } else if (key == BSP_INPUT_NAVIGATION_KEY_DOWN) {
        if (current_view == VIEW_SETTINGS) {
            if (!edit_mode) selected = (selected + 1) % FIELD_COUNT;
            else if (!field_editing_text) field_adjust(selected, -1);
        } else if (current_view == VIEW_NODES) {
            int upper = node_count + contact_count - 1;
            if (upper < 0) upper = 0;
            if (node_cursor < upper) node_cursor++;
        } else if (current_view == VIEW_CHAT && dm_inbox_mode) {
            int upper = (dm_target_set ? 1 : 0) + contact_count - 1;
            if (upper < 0) upper = 0;
            if (dm_inbox_cursor < upper) dm_inbox_cursor++;
        }
    } else if (key == BSP_INPUT_NAVIGATION_KEY_LEFT) {
        if (current_view == VIEW_SETTINGS && edit_mode && !field_editing_text) field_adjust(selected, -1);
    } else if (key == BSP_INPUT_NAVIGATION_KEY_RIGHT) {
        if (current_view == VIEW_SETTINGS && edit_mode && !field_editing_text) field_adjust(selected, +1);
    } else if (key == BSP_INPUT_NAVIGATION_KEY_RETURN) {
        // Inbox: pick a conversation
        if (current_view == VIEW_CHAT && dm_inbox_mode && !chat_typing) {
            int idx_map[MAX_CONTACTS + 1];
            int idx_count = 0;
            bool active_on_top = dm_target_set;
            if (active_on_top) idx_map[idx_count++] = -1;
            for (int i = 0; i < contact_count && idx_count < MAX_CONTACTS + 1; i++) {
                if (active_on_top && memcmp(contacts[i].pub_key, dm_target_pub, MESHCORE_PUB_KEY_SIZE) == 0)
                    continue;
                idx_map[idx_count++] = i;
            }
            if (dm_inbox_cursor < idx_count) {
                int e = idx_map[dm_inbox_cursor];
                if (e >= 0) {
                    dm_select_target(contacts[e].pub_key, contacts[e].alias);
                }
                dm_inbox_mode = false;
                led_dm_pending = false;
                update_notification_led();
            }
            return;
        }
        if ((current_view == VIEW_CHAT || current_view == VIEW_CHANNEL) && chat_typing) {
            if (chat_input_len > 0) {
                if (current_view == VIEW_CHANNEL) {
                    send_chat_message(chat_input);
                    ch_add_message(chat_input, true);
                } else if (dm_target_set) {
                    send_dm_message(chat_input, dm_target_pub);
                    chat_add_dm(chat_input, true, dm_target_pub);
                    // Persist target so it stays in the inbox after reboot.
                    meshcore_device_role_t r = MESHCORE_DEVICE_ROLE_CHAT_NODE;
                    if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                        for (int ni = 0; ni < MAX_NODES; ni++) {
                            if (node_list[ni].active &&
                                memcmp(node_list[ni].pub_key, dm_target_pub, MESHCORE_PUB_KEY_SIZE) == 0) {
                                r = node_list[ni].role; break;
                            }
                        }
                        xSemaphoreGive(node_mutex);
                    }
                    contact_ensure(dm_target_pub, dm_target_name, (uint8_t)r);
                } else {
                    // No DM target: do NOT fall back to channel (was a hidden trap).
                    chat_add_message("(geen DM-target — kies een node in Nodes-tab)", false);
                }
                chat_input_len = 0;
                chat_input[0]  = '\0';
            }
            chat_typing = false;
        } else if (current_view == VIEW_NODES) {
            // Select node/contact under cursor → set DM target, open Chat tab
            if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                display_row_t rows_dl[MAX_CONTACTS + MAX_NODES];
                int idx_count = build_node_display(rows_dl, MAX_CONTACTS + MAX_NODES);
                if (node_cursor < idx_count) {
                    display_row_t *d = &rows_dl[node_cursor];
                    if (d->node_idx >= 0) {
                        node_entry_t *n = &node_list[d->node_idx];
                        dm_select_target(n->pub_key, n->name);
                        contact_ensure(n->pub_key, n->name, (uint8_t)n->role);
                    } else if (d->is_contact) {
                        contact_t *c = &contacts[d->contact_idx];
                        dm_select_target(c->pub_key, c->alias);
                    }
                }
                xSemaphoreGive(node_mutex);
            }
            if (dm_target_set) {
                current_view   = VIEW_CHAT;
                dm_inbox_mode  = false;  // straight into chat view when opened from Nodes
                led_dm_pending = false;
                update_notification_led();
            }
        } else if (current_view == VIEW_SETTINGS) {
            if (!edit_mode) {
                edit_mode = true;
                if (selected == FIELD_OWNER || selected == FIELD_ADV_NAME ||
                    selected == FIELD_REGION_SCOPE) {
                    settings_begin_text_edit(selected);
                }
            } else {
                if (field_editing_text)        settings_commit_text_edit(selected);
                else                            save_lora_config();
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

    // Settings text-edit (FIELD_OWNER / FIELD_ADV_NAME): intercept all printable
    // characters so the global W/S/T/F/L/Q/R/T shortcuts don't fire while typing.
    if (current_view == VIEW_SETTINGS && edit_mode && field_editing_text) {
        if (c == 27) {  // ESC: cancel
            edit_mode          = false;
            field_editing_text = false;
            dirty              = false;
            load_owner_name();
            load_lora_advert_name();
            load_region_scope();
        } else if (c == '\r' || c == '\n') {  // Enter: save
            settings_commit_text_edit(selected);
            edit_mode = false;
            dirty     = false;
        } else if (c == 127 || c == 8) {  // Backspace
            if (field_edit_len > 0) field_edit_buf[--field_edit_len] = '\0';
        } else if (c >= 32 && c < 127 && field_edit_len < (int)sizeof(field_edit_buf) - 1) {
            // Region scope: force lowercase, only [a-z 0-9 -] accepted
            if (selected == FIELD_REGION_SCOPE) {
                if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
                bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
                if (!ok) return;
            }
            field_edit_buf[field_edit_len++] = c;
            field_edit_buf[field_edit_len]   = '\0';
        }
        return;
    }

    // DM inbox view: own keymap (no typing here)
    if (current_view == VIEW_CHAT && dm_inbox_mode && !chat_typing) {
        if (c == 'w' || c == 'W') { if (dm_inbox_cursor > 0) dm_inbox_cursor--; return; }
        if (c == 's' || c == 'S') {
            int upper = (dm_target_set ? 1 : 0) + contact_count - 1;
            if (upper < 0) upper = 0;
            if (dm_inbox_cursor < upper) dm_inbox_cursor++;
            return;
        }
        // T to type only works inside the chat view; here we ignore it.
        if (c == 't' || c == 'T') return;
        // Tab and ESC fall through to common handling below
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
                        chat_add_dm(chat_input, true, dm_target_pub);
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
            if (c == 't' || c == 'T') {
                // T only enters typing mode in chat view (not inbox)
                if (current_view == VIEW_CHANNEL || (current_view == VIEW_CHAT && !dm_inbox_mode)) {
                    chat_typing = true;
                }
                return;
            }
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
            if (current_view == VIEW_CHAT) {
                dm_inbox_mode      = true;   // always land on inbox when entering DM tab
                led_dm_pending     = false;
                update_notification_led();
            }
            if (current_view == VIEW_CHANNEL) { led_channel_pending = false; update_notification_led(); }
        }
        return;
    }

    if (c == 27) {
        if (edit_mode) {
            edit_mode          = false;
            field_editing_text = false;
            dirty              = false;
            load_owner_name();
            load_lora_advert_name();
            load_region_scope();
            load_lora_config();
        } else if (current_view == VIEW_CHAT && !dm_inbox_mode) {
            dm_inbox_mode = true;  // back to inbox list
        } else {
            bsp_led_set_mode(true);
            bsp_device_restart_to_launcher();
        }
    } else if (c == 'w' || c == 'W') {
        if (current_view == VIEW_SETTINGS) {
            if (!edit_mode) selected = (selected - 1 + FIELD_COUNT) % FIELD_COUNT;
            else if (!field_editing_text) field_adjust(selected, +1);
        } else if (current_view == VIEW_NODES) {
            if (node_cursor > 0) node_cursor--;
        }
    } else if (c == 's' || c == 'S') {
        if (current_view == VIEW_SETTINGS) {
            if (!edit_mode) selected = (selected + 1) % FIELD_COUNT;
            else if (!field_editing_text) field_adjust(selected, -1);
        } else if (current_view == VIEW_NODES) {
            int upper = node_count + contact_count - 1;
            if (upper < 0) upper = 0;
            if (node_cursor < upper) node_cursor++;
        }
    } else if ((c == 'a' || c == 'A') && current_view == VIEW_NODES) {
        send_advert();
    } else if ((c == 'f' || c == 'F') && current_view == VIEW_NODES) {
        // Toggle contact status of the row under the cursor
        if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            display_row_t rows_dl[MAX_CONTACTS + MAX_NODES];
            int idx_count = build_node_display(rows_dl, MAX_CONTACTS + MAX_NODES);
            if (node_cursor < idx_count) {
                display_row_t *d = &rows_dl[node_cursor];
                if (d->is_contact) {
                    contact_toggle(contacts[d->contact_idx].pub_key, NULL, 0);
                } else if (d->node_idx >= 0) {
                    node_entry_t *n = &node_list[d->node_idx];
                    int r = contact_toggle(n->pub_key, n->name, (uint8_t)n->role);
                    if (r < 0) ESP_LOGW(TAG, "Contacts list is full (%d/%d)", contact_count, MAX_CONTACTS);
                }
            }
            xSemaphoreGive(node_mutex);
        }
    } else if ((c == 'l' || c == 'L') && current_view == VIEW_NODES) {
        // Cycle filter: ALL → Chat → Repeater → Room Server → Sensor → ALL
        static const meshcore_device_role_t cycle[] = {
            MESHCORE_DEVICE_ROLE_UNKNOWN,
            MESHCORE_DEVICE_ROLE_CHAT_NODE,
            MESHCORE_DEVICE_ROLE_REPEATER,
            MESHCORE_DEVICE_ROLE_ROOM_SERVER,
            MESHCORE_DEVICE_ROLE_SENSOR,
        };
        const int n = (int)(sizeof(cycle) / sizeof(cycle[0]));
        int idx = 0;
        for (int i = 0; i < n; i++) if (cycle[i] == node_filter) { idx = i; break; }
        node_filter = cycle[(idx + 1) % n];
        node_scroll = 0;
        node_cursor = 0;
    } else if ((c == 'q' || c == 'Q') && current_view == VIEW_NODES && identity_ready) {
        qr_overlay_active = true;
    } else if (c == '<' || c == ',') {
        if (current_view == VIEW_SETTINGS && edit_mode && !field_editing_text) field_adjust(selected, -1);
    } else if (c == '>' || c == '.') {
        if (current_view == VIEW_SETTINGS && edit_mode && !field_editing_text) field_adjust(selected, +1);
    } else if (c == '\r' || c == '\n') {
        if (current_view == VIEW_NODES) {
            // Select node/contact under cursor → open DM in Chat tab
            if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                display_row_t rows_dl[MAX_CONTACTS + MAX_NODES];
                int idx_count = build_node_display(rows_dl, MAX_CONTACTS + MAX_NODES);
                if (node_cursor < idx_count) {
                    display_row_t *d = &rows_dl[node_cursor];
                    if (d->node_idx >= 0) {
                        node_entry_t *n = &node_list[d->node_idx];
                        dm_select_target(n->pub_key, n->name);
                        contact_ensure(n->pub_key, n->name, (uint8_t)n->role);
                    } else if (d->is_contact) {
                        contact_t *c2 = &contacts[d->contact_idx];
                        dm_select_target(c2->pub_key, c2->alias);
                    }
                }
                xSemaphoreGive(node_mutex);
            }
            if (dm_target_set) {
                current_view   = VIEW_CHAT;
                dm_inbox_mode  = false;  // straight into chat view when opened from Nodes
                led_dm_pending = false;
                update_notification_led();
            }
        } else if (current_view == VIEW_SETTINGS) {
            if (!edit_mode) {
                edit_mode = true;
                if (selected == FIELD_OWNER || selected == FIELD_ADV_NAME ||
                    selected == FIELD_REGION_SCOPE) {
                    settings_begin_text_edit(selected);
                }
            } else {
                if (field_editing_text)        settings_commit_text_edit(selected);
                else                            save_lora_config();
                edit_mode = false;
                dirty     = false;
            }
        }
    } else if (c == 'r' || c == 'R') {
        if (current_view == VIEW_SETTINGS) {
            load_owner_name();
            load_lora_advert_name();
            load_region_scope();
            load_lora_config();
            dirty              = false;
            edit_mode          = false;
            field_editing_text = false;
        } else if (current_view == VIEW_CHANNEL && !chat_typing) {
            // Wipe channel history (RAM ring + on-disk file)
            if (xSemaphoreTake(ch_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                memset(ch_msgs, 0, sizeof(ch_msgs));
                ch_head    = 0;
                ch_count   = 0;
                ch_scroll  = 0;
                xSemaphoreGive(ch_mutex);
            }
            if (history_ready && xSemaphoreTake(history_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                remove(CH_HISTORY_LOG_PATH);
                xSemaphoreGive(history_mutex);
            }
            ESP_LOGI(TAG, "Channel history cleared by user (R)");
        }
    } else if ((c == 'd' || c == 'D') && current_view == VIEW_CHAT && dm_inbox_mode && !chat_typing) {
        // Delete selected DM conversation: history file + contact from NVS.
        // Contact is re-added automatically on next DM from that peer.
        int idx_map[MAX_CONTACTS + 1];
        int idx_count = 0;
        bool active_on_top = dm_target_set;
        if (active_on_top) idx_map[idx_count++] = -1;
        for (int i = 0; i < contact_count && idx_count < MAX_CONTACTS + 1; i++) {
            if (active_on_top && memcmp(contacts[i].pub_key, dm_target_pub, MESHCORE_PUB_KEY_SIZE) == 0)
                continue;
            idx_map[idx_count++] = i;
        }
        if (dm_inbox_cursor < idx_count) {
            int e = idx_map[dm_inbox_cursor];
            uint8_t target_pub[MESHCORE_PUB_KEY_SIZE];
            char    target_name[MESHCORE_MAX_NAME_SIZE + 1];
            int     ci = -1;
            if (e == -1) {
                memcpy(target_pub, dm_target_pub, MESHCORE_PUB_KEY_SIZE);
                strncpy(target_name, dm_target_name, sizeof(target_name) - 1);
                target_name[sizeof(target_name) - 1] = '\0';
                ci = contact_find(dm_target_pub);
                dm_target_set = false;
                memset(chat_msgs, 0, sizeof(chat_msgs));
                chat_head = chat_count = chat_scroll = 0;
            } else {
                ci = e;
                memcpy(target_pub, contacts[ci].pub_key, MESHCORE_PUB_KEY_SIZE);
                strncpy(target_name, contacts[ci].alias, sizeof(target_name) - 1);
                target_name[sizeof(target_name) - 1] = '\0';
            }

            if (history_ready && xSemaphoreTake(history_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                char path[64];
                history_dm_path(target_pub, path, sizeof(path));
                remove(path);
                xSemaphoreGive(history_mutex);
            }
            if (ci >= 0) {
                for (int j = ci; j < contact_count - 1; j++) contacts[j] = contacts[j + 1];
                memset(&contacts[contact_count - 1], 0, sizeof(contact_t));
                contact_count--;
                save_contacts();
            }
            if (dm_inbox_cursor > 0) dm_inbox_cursor--;
            ESP_LOGI(TAG, "DM deleted by user (D): %s", target_name);
        }
    } else if ((c == 'u' || c == 'U') && !edit_mode && current_view == VIEW_SETTINGS) {
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

    int  diag_y    = 50;
    int  diag_line = 22;
#define DIAG(col, fmt, ...) do { \
        char _buf[80]; \
        snprintf(_buf, sizeof(_buf), fmt, ##__VA_ARGS__); \
        ESP_LOGI(TAG, "%s", _buf); \
        pax_draw_text(&fb, (col), FONT, TXT_SMALL, 14, diag_y, _buf); \
        diag_y += diag_line; \
        blit(); \
    } while(0)

    pax_background(&fb, COL_BG);
    pax_draw_text(&fb, COL_AMBER, FONT, TXT_TITLE, 14, 16, "MeshCore v4");
    blit();
    vTaskDelay(pdMS_TO_TICKS(1500));

    // CET/CEST with EU DST: last Sun Mar 02:00 -> CEST(+2), last Sun Oct 03:00 -> CET(+1)
    // Must be set before settimeofday/localtime_r so all paths show local time.
    // Epoch in NVS stays UTC; tzset handles the local conversion.
    setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
    tzset();

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
    load_lora_advert_name();
    load_region_scope();
    load_contacts();

    DIAG(COL_GRAY, "SD mount...");
    history_init();
    DIAG(history_ready ? COL_GREEN : COL_YELLOW, "  SD: %s", history_status);
    // DM history is loaded per peer in dm_select_target — no boot-time DM load.
    if (history_ready) { history_load_channel(); }

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
                xTaskCreate(lora_rx_task,     "lora_rx",     10240, NULL, 5, NULL);
                xTaskCreate(advert_task,      "lora_advert",  6144, NULL, 4, NULL);
                xTaskCreate(noise_floor_task, "noise_poll",   3072, NULL, 3, NULL);  // 60s interval
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
