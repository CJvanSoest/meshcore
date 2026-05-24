// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "chat.h"

#include <string.h>

#include "freertos/task.h"

#include "esp_log.h"
#include "mbedtls/sha256.h"

#include "bsp/tanmatsu.h"
#include "tanmatsu_coprocessor.h"

#include "history.h"

static const char *TAG = "chat";

// ── State ────────────────────────────────────────────────────────────────────
chat_msg_t        chat_msgs[MAX_CHAT_MSGS];
int               chat_head   = 0;
int               chat_count  = 0;
int               chat_scroll = 0;
SemaphoreHandle_t chat_mutex  = NULL;

chat_msg_t        ch_msgs[MAX_CHAT_MSGS];
int               ch_head     = 0;
int               ch_count    = 0;
int               ch_scroll   = 0;
SemaphoreHandle_t ch_mutex    = NULL;

char chat_input[MAX_INPUT_LEN + 1] = {0};
int  chat_input_len                = 0;
bool chat_typing                   = false;

bool dm_inbox_mode   = true;
int  dm_inbox_cursor = 0;
int  dm_inbox_scroll = 0;

bool    dm_target_set = false;
uint8_t dm_target_pub[MESHCORE_PUB_KEY_SIZE]            = {0};
char    dm_target_name[MESHCORE_MAX_NAME_SIZE + 1]      = {0};

bool led_dm_pending      = false;
bool led_channel_pending = false;

const uint8_t PUBLIC_CHANNEL_KEY[16] = {
    0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a,
    0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72
};
uint8_t channel_hash = 0;

// Replace UTF-8 multi-byte sequences (e.g. emoji) with '?' so the ASCII-only
// font renders something visible. Continuation bytes are silently skipped — the
// lead byte emits one placeholder per sequence.
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

void chat_init(void) {
    if (chat_mutex == NULL) chat_mutex = xSemaphoreCreateMutex();
    if (ch_mutex   == NULL) ch_mutex   = xSemaphoreCreateMutex();

    uint8_t digest[32];
    mbedtls_sha256(PUBLIC_CHANNEL_KEY, sizeof(PUBLIC_CHANNEL_KEY), digest, 0);
    channel_hash = digest[0];
    ESP_LOGI(TAG, "Channel hash: 0x%02X", channel_hash);
}

// RAM-only: for system/error messages without peer context.
// NOT persisted (would otherwise pollute every peer's chat).
void chat_add_message(const char *text, bool is_mine) {
    if (xSemaphoreTake(chat_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    chat_msg_t *m   = &chat_msgs[chat_head];
    m->active       = true;
    m->is_mine      = is_mine;
    m->timestamp_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    utf8_sanitize(m->text, MAX_MSG_TEXT, text);
    chat_head = (chat_head + 1) % MAX_CHAT_MSGS;
    if (chat_count < MAX_CHAT_MSGS) chat_count++;
    chat_scroll = chat_count;
    xSemaphoreGive(chat_mutex);
}

// DM with peer context: always persist to peer's file; only add to ring if that
// peer is currently active.
void chat_add_dm(const char *text, bool is_mine, const uint8_t peer_pub[32]) {
    if (peer_pub) history_append_dm(peer_pub, text, is_mine);
    if (dm_target_set && peer_pub &&
        memcmp(peer_pub, dm_target_pub, MESHCORE_PUB_KEY_SIZE) == 0) {
        chat_add_message(text, is_mine);
    }
}

// Switch the active DM peer + reload its history from SD.
void dm_select_target(const uint8_t pub[32], const char *name) {
    dm_target_set = true;
    memcpy(dm_target_pub, pub, MESHCORE_PUB_KEY_SIZE);
    strncpy(dm_target_name, name ? name : "", sizeof(dm_target_name) - 1);
    dm_target_name[sizeof(dm_target_name) - 1] = '\0';
    if (xSemaphoreTake(chat_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        memset(chat_msgs, 0, sizeof(chat_msgs));
        chat_head   = 0;
        chat_count  = 0;
        chat_scroll = 0;
        xSemaphoreGive(chat_mutex);
    }
    history_load_dm(pub, chat_ring_add_from_disk);
}

void ch_add_message(const char *text, bool is_mine) {
    if (xSemaphoreTake(ch_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    chat_msg_t *m   = &ch_msgs[ch_head];
    m->active       = true;
    m->is_mine      = is_mine;
    m->timestamp_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    utf8_sanitize(m->text, MAX_MSG_TEXT, text);
    ch_head = (ch_head + 1) % MAX_CHAT_MSGS;
    if (ch_count < MAX_CHAT_MSGS) ch_count++;
    ch_scroll = ch_count;
    xSemaphoreGive(ch_mutex);

    history_append_channel(m->text, is_mine);
}

void update_notification_led(void) {
    tanmatsu_coprocessor_handle_t copr = NULL;
    if (bsp_tanmatsu_coprocessor_get_handle(&copr) != ESP_OK) return;
    if (led_dm_pending) {
        tanmatsu_coprocessor_set_message(copr, false, true, false, false, false, false, false, false);
    } else if (led_channel_pending) {
        tanmatsu_coprocessor_set_message(copr, false, false, true, false, false, false, false, false);
    } else {
        tanmatsu_coprocessor_set_message(copr, false, false, false, false, false, false, false, false);
    }
}

void chat_ring_add_from_disk(const char *text, bool is_mine) {
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

void ch_ring_add_from_disk(const char *text, bool is_mine) {
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
