// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
// SPDX-FileContributor: Ilias el Matani <hello@ilias.codes>

#include "chat.h"
#include <string.h>
#include <time.h>
#include "bsp/tanmatsu.h"
#include "channels.h"
#include "contacts.h"
#include "emoji_table.h"  // codepoint lookup + utf8_decode (pax-free; no UI dep)
#include "esp_log.h"
#include "freertos/task.h"
#include "history.h"
#include "mbedtls/sha256.h"
#include "tanmatsu_coprocessor.h"

static const char* TAG = "chat";

// ── State ────────────────────────────────────────────────────────────────────
chat_msg_t        chat_msgs[MAX_CHAT_MSGS];
int               chat_head   = 0;
int               chat_count  = 0;
int               chat_scroll = 0;
SemaphoreHandle_t chat_mutex  = NULL;

chat_msg_t        ch_msgs[MAX_CHAT_MSGS];
int               ch_head   = 0;
int               ch_count  = 0;
int               ch_scroll = 0;
SemaphoreHandle_t ch_mutex  = NULL;

char chat_input[MAX_INPUT_LEN + 1] = {0};
int  chat_input_len                = 0;
bool chat_typing                   = false;

bool emoji_picker_active = false;
int  emoji_picker_cursor = 0;

bool dm_inbox_mode   = true;
int  dm_inbox_cursor = 0;
int  dm_inbox_scroll = 0;

bool    dm_target_set                              = false;
uint8_t dm_target_pub[MESHCORE_PUB_KEY_SIZE]       = {0};
char    dm_target_name[MESHCORE_MAX_NAME_SIZE + 1] = {0};

bool led_dm_pending      = false;
bool led_channel_pending = false;

const uint8_t PUBLIC_CHANNEL_KEY[16] = {0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a,
                                        0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72};
uint8_t       channel_hash           = 0;

// Preserve UTF-8 sequences that decode to a codepoint we have an inline emoji
// glyph for. Unknown multi-byte sequences collapse to a single '?' so the
// ASCII-only Saira font still renders them as something.
static void utf8_sanitize(char* dst, size_t dst_sz, const char* src) {
    size_t di = 0;
    size_t si = 0;
    while (src[si] && di + 1 < dst_sz) {
        unsigned char c = (unsigned char)src[si];
        if (c < 0x80) {
            dst[di++] = (char)c;
            si++;
            continue;
        }
        uint32_t cp  = 0;
        int      adv = utf8_decode(&src[si], &cp);
        if (adv > 0 && emoji_lookup_by_codepoint(cp) >= 0 && di + adv + 1 < dst_sz) {
            // Recognised emoji: copy the full UTF-8 sequence through.
            memcpy(&dst[di], &src[si], (size_t)adv);
            di += (size_t)adv;
            si += (size_t)adv;
            continue;
        }
        // Unknown / malformed multi-byte: emit one '?', skip the lead byte and
        // any continuation bytes that follow.
        dst[di++]  = '?';
        si        += (adv > 0) ? (size_t)adv : 1;
    }
    dst[di] = '\0';
}

void chat_init(void) {
    if (chat_mutex == NULL) chat_mutex = xSemaphoreCreateMutex();
    if (ch_mutex == NULL) ch_mutex = xSemaphoreCreateMutex();

    uint8_t digest[32];
    mbedtls_sha256(PUBLIC_CHANNEL_KEY, sizeof(PUBLIC_CHANNEL_KEY), digest, 0);
    channel_hash = digest[0];
    ESP_LOGI(TAG, "Channel hash: 0x%02X", channel_hash);
}

// Fill metadata fields that every fresh-add path wants. Lives separately so
// neither the disk-replay nor the system-message path need to repeat it.
static void msg_init_meta(chat_msg_t* m, bool is_mine) {
    m->timestamp_ms   = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    time_t now        = time(NULL);
    m->timestamp_unix = (now > 1000000000) ? (uint32_t)now : 0;
    m->hops           = is_mine ? 0 : 0xFF;  // theirs: filled in by chat_set_meta_*
    m->ack_state      = 0;
    memset(m->ack_crc, 0, sizeof(m->ack_crc));
}

// RAM-only: for system/error messages without peer context.
// NOT persisted (would otherwise pollute every peer's chat).
void chat_add_message(const char* text, bool is_mine) {
    if (xSemaphoreTake(chat_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    chat_msg_t* m = &chat_msgs[chat_head];
    memset(m, 0, sizeof(*m));
    m->active  = true;
    m->is_mine = is_mine;
    msg_init_meta(m, is_mine);
    utf8_sanitize(m->text, MAX_MSG_TEXT, text);
    chat_head = (chat_head + 1) % MAX_CHAT_MSGS;
    if (chat_count < MAX_CHAT_MSGS) chat_count++;
    chat_scroll = chat_count;
    xSemaphoreGive(chat_mutex);
}

// DM with peer context: always persist to peer's file; only add to ring if that
// peer is currently active.
void chat_add_dm(const char* text, bool is_mine, const uint8_t peer_pub[32]) {
    if (peer_pub) history_append_dm(peer_pub, text, is_mine);
    if (dm_target_set && peer_pub && memcmp(peer_pub, dm_target_pub, MESHCORE_PUB_KEY_SIZE) == 0) {
        chat_add_message(text, is_mine);
    }
}

// Switch the active DM peer + reload its history from SD.
void dm_select_target(const uint8_t pub[32], const char* name) {
    dm_target_set = true;
    memcpy(dm_target_pub, pub, MESHCORE_PUB_KEY_SIZE);
    strncpy(dm_target_name, name ? name : "", sizeof(dm_target_name) - 1);
    dm_target_name[sizeof(dm_target_name) - 1] = '\0';
    // Capture the unread count *before* clearing it so we can park the
    // scroll on the first unread message after history reload.
    int unread_before                          = 0;
    int ci                                     = contact_find(pub);
    if (ci >= 0) unread_before = contact_unread[ci];
    contact_clear_unread(pub);  // opening a conversation clears its unread
    update_notification_led();  // turn LED off once nothing remains unread
    if (xSemaphoreTake(chat_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        memset(chat_msgs, 0, sizeof(chat_msgs));
        chat_head   = 0;
        chat_count  = 0;
        chat_scroll = 0;
        xSemaphoreGive(chat_mutex);
    }
    history_load_dm(pub, chat_ring_add_from_disk);
    // After history load, ch_ring_add_from_disk has left chat_scroll =
    // chat_count (newest at the bottom). If the user had unread messages,
    // park the scroll on the first unread instead so they don't have to
    // back-scroll to find what they missed.
    if (unread_before > 0 && unread_before < chat_count) {
        if (xSemaphoreTake(chat_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            chat_scroll = chat_count - unread_before + 1;
            if (chat_scroll < 1) chat_scroll = 1;
            if (chat_scroll > chat_count) chat_scroll = chat_count;
            xSemaphoreGive(chat_mutex);
        }
    }
}

// Channel message with explicit channel context: always persist to that
// channel's own history file; only push onto the visible ring if that channel
// is the active one. Returns true if it was added to the visible ring (so the
// RX path knows whether per-message meta applies). ch_idx out of range is
// treated as "persist nowhere, ring only" — shouldn't happen in practice.
bool ch_add_message_for(int ch_idx, const char* text, bool is_mine) {
    char sanitized[MAX_MSG_TEXT];
    utf8_sanitize(sanitized, MAX_MSG_TEXT, text);

    if (ch_idx >= 0 && ch_idx < CHANNELS_MAX && channels[ch_idx].active) {
        history_append_channel(channels[ch_idx].secret, sanitized, is_mine);
    }

    bool to_ring = (ch_idx == active_channel_idx);
    if (!to_ring) return false;

    if (xSemaphoreTake(ch_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    chat_msg_t* m = &ch_msgs[ch_head];
    memset(m, 0, sizeof(*m));
    m->active  = true;
    m->is_mine = is_mine;
    msg_init_meta(m, is_mine);
    strncpy(m->text, sanitized, MAX_MSG_TEXT - 1);
    m->text[MAX_MSG_TEXT - 1] = '\0';
    ch_head                   = (ch_head + 1) % MAX_CHAT_MSGS;
    if (ch_count < MAX_CHAT_MSGS) ch_count++;
    ch_scroll = ch_count;
    xSemaphoreGive(ch_mutex);
    return true;
}

// Convenience for own outgoing messages — always target the active channel.
void ch_add_message(const char* text, bool is_mine) {
    ch_add_message_for(active_channel_idx, text, is_mine);
}

// Switch the active channel + reload its history from SD into the visible ring.
void ch_select_channel(int idx) {
    if (idx < 0 || idx >= CHANNELS_MAX || !channels[idx].active) return;
    active_channel_idx  = idx;
    int unread_before   = channel_unread[idx];  // capture before we clear
    channel_unread[idx] = 0;                    // opening a channel clears its unread
    update_notification_led();                  // turn LED off once nothing remains unread
    if (xSemaphoreTake(ch_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        memset(ch_msgs, 0, sizeof(ch_msgs));
        ch_head   = 0;
        ch_count  = 0;
        ch_scroll = 0;
        xSemaphoreGive(ch_mutex);
    }
    history_load_channel(channels[idx].secret, ch_ring_add_from_disk);
    // Same trick as dm_select_target: after history reload, park the scroll
    // on the first unread message so the user starts reading where they
    // left off instead of at the newest message.
    if (unread_before > 0 && unread_before < ch_count) {
        if (xSemaphoreTake(ch_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            ch_scroll = ch_count - unread_before + 1;
            if (ch_scroll < 1) ch_scroll = 1;
            if (ch_scroll > ch_count) ch_scroll = ch_count;
            xSemaphoreGive(ch_mutex);
        }
    }
}

void update_notification_led(void) {
    tanmatsu_coprocessor_handle_t copr = NULL;
    if (bsp_tanmatsu_coprocessor_get_handle(&copr) != ESP_OK) return;
    // Driven by the live unread totals (not the old led_*_pending flags) so the
    // LED clears the moment the last unread DM/channel is opened — no tab cycling.
    bool dm = (contact_unread_total() > 0);
    bool ch = (channel_unread_total() > 0);
    if (dm) {
        tanmatsu_coprocessor_set_message(copr, false, true, false, false, false, false, false, false);
    } else if (ch) {
        tanmatsu_coprocessor_set_message(copr, false, false, true, false, false, false, false, false);
    } else {
        tanmatsu_coprocessor_set_message(copr, false, false, false, false, false, false, false, false);
    }
}

void chat_ring_add_from_disk(const char* text, bool is_mine) {
    if (xSemaphoreTake(chat_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    chat_msg_t* m = &chat_msgs[chat_head];
    memset(m, 0, sizeof(*m));
    m->active       = true;
    m->is_mine      = is_mine;
    m->timestamp_ms = 0;
    m->hops         = 0xFF;  // unknown — history records pre-date this field
    utf8_sanitize(m->text, MAX_MSG_TEXT, text);
    chat_head = (chat_head + 1) % MAX_CHAT_MSGS;
    if (chat_count < MAX_CHAT_MSGS) chat_count++;
    chat_scroll = chat_count;
    xSemaphoreGive(chat_mutex);
}

void ch_ring_add_from_disk(const char* text, bool is_mine) {
    if (xSemaphoreTake(ch_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    chat_msg_t* m = &ch_msgs[ch_head];
    memset(m, 0, sizeof(*m));
    m->active       = true;
    m->is_mine      = is_mine;
    m->timestamp_ms = 0;
    m->hops         = 0xFF;
    utf8_sanitize(m->text, MAX_MSG_TEXT, text);
    ch_head = (ch_head + 1) % MAX_CHAT_MSGS;
    if (ch_count < MAX_CHAT_MSGS) ch_count++;
    ch_scroll = ch_count;
    xSemaphoreGive(ch_mutex);
}

// "most recent" = the entry just appended, i.e. (head-1) wrapping. We do not
// take the mutex here on the assumption that the caller is on the same task
// path that just appended and the lock has been released — the small race
// window of another writer racing in between add() and set_meta() is acceptable
// (metadata might land on the next entry, which is cosmetic only).
static int last_idx(int head) {
    return (head - 1 + MAX_CHAT_MSGS) % MAX_CHAT_MSGS;
}

void chat_set_meta_dm(uint8_t hops) {
    if (chat_count == 0) return;
    chat_msgs[last_idx(chat_head)].hops = hops;
}

void chat_set_meta_channel(uint8_t hops) {
    if (ch_count == 0) return;
    ch_msgs[last_idx(ch_head)].hops = hops;
}

void chat_arm_ack_dm(const uint8_t ack_crc[4]) {
    // Writes ack_state/ack_crc that the RX task reads under chat_mutex in
    // chat_mark_ack_by_crc; take the lock so that scan never sees a torn crc.
    if (xSemaphoreTake(chat_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    if (chat_count > 0) {
        chat_msg_t* m = &chat_msgs[last_idx(chat_head)];
        if (m->is_mine) {
            m->ack_state = 1;
            memcpy(m->ack_crc, ack_crc, 4);
        }
    }
    xSemaphoreGive(chat_mutex);
}

bool chat_mark_ack_by_crc(const uint8_t ack_crc[4]) {
    if (xSemaphoreTake(chat_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    bool hit = false;
    for (int i = 0; i < chat_count; i++) {
        int         idx = (chat_head - 1 - i + MAX_CHAT_MSGS * 2) % MAX_CHAT_MSGS;
        chat_msg_t* m   = &chat_msgs[idx];
        if (m->is_mine && m->ack_state == 1 && memcmp(m->ack_crc, ack_crc, 4) == 0) {
            m->ack_state = 2;
            hit          = true;
            break;
        }
    }
    xSemaphoreGive(chat_mutex);
    return hit;
}

// ── Channel relay confirmation (mirrors the DM ACK helpers above) ─────────────
// A channel (GRP_TXT) broadcast has no recipient to ACK, but hearing our own
// flood echoed back by a neighbour proves it was relayed into the mesh. We track
// that as ack_state on the visible-ring entry: 1 = sent, 2 = relayed, 3 = not
// sent. Best-effort and RAM-only — switching channels rebuilds the ring.
void ch_arm_relay(const uint8_t fp[4]) {
    if (xSemaphoreTake(ch_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    if (ch_count > 0) {
        chat_msg_t* m = &ch_msgs[last_idx(ch_head)];
        if (m->is_mine) {
            m->ack_state = 1;
            memcpy(m->ack_crc, fp, 4);
        }
    }
    xSemaphoreGive(ch_mutex);
}

bool ch_mark_relayed_by_fp(const uint8_t fp[4]) {
    if (xSemaphoreTake(ch_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    bool hit = false;
    for (int i = 0; i < ch_count; i++) {
        int         idx = (ch_head - 1 - i + MAX_CHAT_MSGS * 2) % MAX_CHAT_MSGS;
        chat_msg_t* m   = &ch_msgs[idx];
        if (m->is_mine && m->ack_state == 1 && memcmp(m->ack_crc, fp, 4) == 0) {
            m->ack_state = 2;
            hit          = true;
            break;
        }
    }
    xSemaphoreGive(ch_mutex);
    return hit;
}

void ch_mark_not_sent(void) {
    if (xSemaphoreTake(ch_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    if (ch_count > 0) {
        chat_msg_t* m = &ch_msgs[last_idx(ch_head)];
        if (m->is_mine) m->ack_state = 3;
    }
    xSemaphoreGive(ch_mutex);
}
