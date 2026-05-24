// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "history.h"
#include "app_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "mbedtls/aes.h"
#include "mbedtls/md.h"

// Tanmatsu µSD pins — slot 0 (slot 1 = hosted Wi-Fi link).
#define MC_SDCARD_CLK 43
#define MC_SDCARD_CMD 44
#define MC_SDCARD_D0  39
#define MC_SDCARD_D1  40
#define MC_SDCARD_D2  41
#define MC_SDCARD_D3  42

#define SD_MOUNT_POINT       "/sd"
#define HISTORY_DIR          "/sd/meshcore"
#define HISTORY_DM_DIR       "/sd/meshcore/dm"
#define CH_HISTORY_LOG_PATH  "/sd/meshcore/channel.bin"
#define HISTORY_REC_MAGIC    "MCR1"

static const char *TAG = "history";

static bool              s_ready  = false;
static const char       *s_status = "off";
static uint8_t           s_key[32] = {0};
static SemaphoreHandle_t s_mutex   = NULL;
static sdmmc_card_t     *s_card    = NULL;

typedef struct __attribute__((packed)) {
    uint8_t  magic[4];      // "MCR1"
    uint16_t plain_len;     // 1..MAX_MSG_TEXT
    uint8_t  flags;         // bit0 = is_mine
    uint8_t  reserved;
    uint32_t ts_unix;       // seconds since epoch (LE)
    uint8_t  iv[16];
} history_rec_hdr_t;          // 28 bytes

static esp_err_t mount_sd(void) {
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
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
        .max_files              = 4,
        .allocation_unit_size   = 16 * 1024,
    };
    return esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mnt, &s_card);
}

void history_init(const uint8_t prv_key[32]) {
    s_mutex = xSemaphoreCreateMutex();

    esp_err_t e = mount_sd();
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s — chat history disabled", esp_err_to_name(e));
        s_status = (e == ESP_ERR_NOT_FOUND || e == ESP_ERR_TIMEOUT) ? "no-sd" : "err";
        return;
    }
    ESP_LOGI(TAG, "SD mounted at %s", SD_MOUNT_POINT);

    mkdir(HISTORY_DIR, 0775);     // ignores EEXIST
    mkdir(HISTORY_DM_DIR, 0775);

    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                    prv_key, 32,
                    (const uint8_t *)"mc-history-v1", 13,
                    s_key);

    s_ready  = true;
    s_status = "ok";
}

const char *history_status(void)  { return s_status; }
bool        history_is_ready(void){ return s_ready;  }

static void append_impl(const char *path, const char *text, bool is_mine) {
    if (!s_ready || text == NULL) return;
    int N = (int)strnlen(text, MAX_MSG_TEXT);
    if (N <= 0) return;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return;

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
    mbedtls_aes_setkey_enc(&aes, s_key, 128);
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
        ESP_LOGW(TAG, "append: fopen(%s) failed", path);
    }
    xSemaphoreGive(s_mutex);
}

static void load_impl(const char *path, history_ring_add_fn add) {
    if (!s_ready || add == NULL) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return;

    FILE *f = fopen(path, "rb");
    if (!f) { xSemaphoreGive(s_mutex); return; }

    int  loaded = 0;
    bool fatal  = false;
    while (1) {
        history_rec_hdr_t hdr;
        if (fread(&hdr, sizeof(hdr), 1, f) != 1) break;
        if (memcmp(hdr.magic, HISTORY_REC_MAGIC, 4) != 0) {
            ESP_LOGW(TAG, "(%s): bad magic at rec %d — stop", path, loaded);
            fatal = true; break;
        }
        if (hdr.plain_len == 0 || hdr.plain_len > MAX_MSG_TEXT) {
            ESP_LOGW(TAG, "(%s): bad len %u at rec %d — stop", path, hdr.plain_len, loaded);
            fatal = true; break;
        }
        int padded = ((hdr.plain_len + 16) / 16) * 16;
        uint8_t ct[MAX_MSG_TEXT + 32];
        if (fread(ct, padded, 1, f) != 1) break;

        uint8_t pt[MAX_MSG_TEXT + 32];
        mbedtls_aes_context aes;
        mbedtls_aes_init(&aes);
        mbedtls_aes_setkey_dec(&aes, s_key, 128);
        mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, padded, hdr.iv, ct, pt);
        mbedtls_aes_free(&aes);

        uint8_t pad = pt[padded - 1];
        if (pad == 0 || pad > 16 || (padded - pad) != hdr.plain_len) {
            ESP_LOGW(TAG, "(%s): decrypt mismatch at rec %d — stop", path, loaded);
            fatal = true; break;
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

    // Self-heal: a file that cannot be parsed at all (likely written with a
    // previous identity key) blocks all future loads. Wipe it so the next
    // append starts a fresh log under the current key.
    if (fatal && loaded == 0) {
        ESP_LOGW(TAG, "(%s): unreadable from start — removing stale file", path);
        remove(path);
    }

    ESP_LOGI(TAG, "load(%s): %d record(s) restored", path, loaded);
    xSemaphoreGive(s_mutex);
}

void history_append_channel(const char *text, bool is_mine) {
    append_impl(CH_HISTORY_LOG_PATH, text, is_mine);
}

void history_load_channel(history_ring_add_fn add) {
    load_impl(CH_HISTORY_LOG_PATH, add);
}

// Per-contact path: /sd/meshcore/dm/<pubkey-hex16>.bin
// 8-byte pubkey prefix as hex = 16 chars, unique enough for max 16 contacts.
static void dm_path(const uint8_t pub[32], char *out, size_t cap) {
    snprintf(out, cap, "%s/%02x%02x%02x%02x%02x%02x%02x%02x.bin",
             HISTORY_DM_DIR,
             pub[0], pub[1], pub[2], pub[3], pub[4], pub[5], pub[6], pub[7]);
}

void history_append_dm(const uint8_t peer_pub[32], const char *text, bool is_mine) {
    if (!s_ready || peer_pub == NULL) return;
    char path[64];
    dm_path(peer_pub, path, sizeof(path));
    append_impl(path, text, is_mine);
}

void history_load_dm(const uint8_t peer_pub[32], history_ring_add_fn add) {
    if (!s_ready || peer_pub == NULL) return;
    char path[64];
    dm_path(peer_pub, path, sizeof(path));
    load_impl(path, add);
}

void history_delete_channel(void) {
    if (!s_ready) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return;
    remove(CH_HISTORY_LOG_PATH);
    xSemaphoreGive(s_mutex);
}

void history_delete_dm(const uint8_t peer_pub[32]) {
    if (!s_ready || peer_pub == NULL) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return;
    char path[64];
    dm_path(peer_pub, path, sizeof(path));
    remove(path);
    xSemaphoreGive(s_mutex);
}
