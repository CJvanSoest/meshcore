// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "channels.h"

#include <string.h>

#include "esp_log.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

static const char *TAG = "channels";

#define NVS_NAMESPACE "system"
#define NVS_KEY       "mc.channels"

// Upstream MeshCore PUBLIC_GROUP_PSK ("izOH6cXN6mrJ5e26oRXNcg==" base64-decoded).
// Hardcoded; same value lives in chat.c for backwards compat.
static const uint8_t PUBLIC_GROUP_PSK[16] = {
    0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a,
    0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72,
};

channel_t channels[CHANNELS_MAX] = {0};
int       channel_count          = 0;
int       active_channel_idx     = 0;

bool channel_list_mode    = true;
int  channel_list_cursor  = 0;
bool channel_adding       = false;

static void compute_hash(channel_t *ch) {
    uint8_t digest[32];
    mbedtls_sha256(ch->secret, CHANNEL_SECRET_LEN, digest, 0);
    ch->hash = digest[0];
}

void channels_derive_secret_from_name(const char *name, uint8_t out_secret[CHANNEL_SECRET_LEN]) {
    uint8_t digest[32];
    mbedtls_sha256((const uint8_t *)name, strlen(name), digest, 0);
    memcpy(out_secret, digest, CHANNEL_SECRET_LEN);
}

static void bootstrap_public(void) {
    channel_t *p = &channels[0];
    p->active = true;
    strcpy(p->name, "Public");
    memcpy(p->secret, PUBLIC_GROUP_PSK, CHANNEL_SECRET_LEN);
    compute_hash(p);
    if (channel_count < 1) channel_count = 1;
    ESP_LOGI(TAG, "Bootstrap Public: hash=0x%02X", p->hash);
}

// On-disk record layout (packed, little endian):
//   uint8_t count
//   for i in [0..count):
//     char    name[CHANNEL_NAME_MAX_LEN + 1]  (null-padded)
//     uint8_t secret[CHANNEL_SECRET_LEN]
typedef struct __attribute__((packed)) {
    char    name[CHANNEL_NAME_MAX_LEN + 1];
    uint8_t secret[CHANNEL_SECRET_LEN];
} stored_channel_t;

static void load_from_nvs(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;

    size_t blob_size = 0;
    esp_err_t res = nvs_get_blob(h, NVS_KEY, NULL, &blob_size);
    if (res != ESP_OK || blob_size < 1) {
        nvs_close(h);
        return;
    }
    uint8_t buf[1 + (CHANNELS_MAX - 1) * sizeof(stored_channel_t)];
    if (blob_size > sizeof(buf)) blob_size = sizeof(buf);
    if (nvs_get_blob(h, NVS_KEY, buf, &blob_size) != ESP_OK) {
        nvs_close(h);
        return;
    }
    nvs_close(h);

    uint8_t count = buf[0];
    if (count > CHANNELS_MAX - 1) count = CHANNELS_MAX - 1;
    stored_channel_t const *src = (stored_channel_t const *)(buf + 1);
    for (uint8_t i = 0; i < count; i++) {
        if ((size_t)(1 + (i + 1) * sizeof(stored_channel_t)) > blob_size) break;
        channel_t *dst = &channels[1 + i];
        dst->active   = true;
        memcpy(dst->name,   src[i].name,   sizeof(dst->name));
        dst->name[CHANNEL_NAME_MAX_LEN] = '\0';
        memcpy(dst->secret, src[i].secret, CHANNEL_SECRET_LEN);
        compute_hash(dst);
        channel_count = 1 + i + 1;
        ESP_LOGI(TAG, "Loaded channel[%d] %s hash=0x%02X", 1 + i, dst->name, dst->hash);
    }
}

void channels_save_nvs(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;

    uint8_t buf[1 + (CHANNELS_MAX - 1) * sizeof(stored_channel_t)] = {0};
    uint8_t  count = 0;
    stored_channel_t *dst = (stored_channel_t *)(buf + 1);
    for (int i = 1; i < channel_count && i < CHANNELS_MAX; i++) {
        if (!channels[i].active) continue;
        memcpy(dst[count].name,   channels[i].name,   sizeof(dst[count].name));
        memcpy(dst[count].secret, channels[i].secret, CHANNEL_SECRET_LEN);
        count++;
    }
    buf[0] = count;
    size_t blob_size = 1 + count * sizeof(stored_channel_t);
    if (nvs_set_blob(h, NVS_KEY, buf, blob_size) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "Saved %u user channels to NVS", (unsigned)count);
}

void channels_init(void) {
    memset(channels, 0, sizeof(channels));
    channel_count      = 0;
    active_channel_idx = 0;
    bootstrap_public();
    load_from_nvs();

    for (int i = 0; i < channel_count; i++) {
        if (!channels[i].active) continue;
        ESP_LOGI(TAG, "channel[%d] %-12s hash=0x%02x", i, channels[i].name, channels[i].hash);
    }
}

int channels_find_by_hash(uint8_t hash) {
    for (int i = 0; i < channel_count; i++) {
        if (channels[i].active && channels[i].hash == hash) return i;
    }
    return -1;
}

static int find_free_slot(void) {
    for (int i = 1; i < CHANNELS_MAX; i++) {
        if (!channels[i].active) return i;
    }
    return -1;
}

static int find_by_secret(uint8_t const secret[CHANNEL_SECRET_LEN]) {
    for (int i = 0; i < CHANNELS_MAX; i++) {
        if (channels[i].active && memcmp(channels[i].secret, secret, CHANNEL_SECRET_LEN) == 0)
            return i;
    }
    return -1;
}

int channels_add_with_secret(const char *name, const uint8_t secret[CHANNEL_SECRET_LEN]) {
    if (!name || !secret) return -1;
    int dup = find_by_secret(secret);
    if (dup >= 0) return dup;
    int slot = find_free_slot();
    if (slot < 0) return -1;
    channel_t *ch = &channels[slot];
    ch->active = true;
    strncpy(ch->name, name, CHANNEL_NAME_MAX_LEN);
    ch->name[CHANNEL_NAME_MAX_LEN] = '\0';
    memcpy(ch->secret, secret, CHANNEL_SECRET_LEN);
    compute_hash(ch);
    if (slot + 1 > channel_count) channel_count = slot + 1;
    channels_save_nvs();
    ESP_LOGI(TAG, "Added channel[%d] %s hash=0x%02X", slot, ch->name, ch->hash);
    return slot;
}

int channels_add_by_name(const char *name) {
    if (!name || !name[0]) return -1;
    uint8_t secret[CHANNEL_SECRET_LEN];
    channels_derive_secret_from_name(name, secret);
    return channels_add_with_secret(name, secret);
}

bool channels_remove(int idx) {
    if (idx <= 0 || idx >= CHANNELS_MAX) return false;  // Public cannot be removed
    if (!channels[idx].active) return false;
    memset(&channels[idx], 0, sizeof(channels[idx]));
    // Compact channel_count
    int last = 0;
    for (int i = 0; i < CHANNELS_MAX; i++) {
        if (channels[i].active) last = i;
    }
    channel_count = last + 1;
    if (active_channel_idx == idx) active_channel_idx = 0;
    channels_save_nvs();
    return true;
}
