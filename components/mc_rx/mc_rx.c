// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
// SPDX-FileContributor: Ilias el Matani <hello@ilias.codes>
//
// RX application layer (Break A): the MeshCore receive handlers, lifted out of
// radio.c so the radio transport stays domain-free. radio.c deserializes +
// dedups and calls the sink registered here; this layer owns decrypt (via
// mc_crypto), the domain writes (chat/contacts/nodes/channels), the UI
// notifications, and the PATH_RETURN ACK (sent through radio_tx_message).

#include "mc_rx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "advert_sign.h"
#include "app_config.h"
#include "channels.h"
#include "chat.h"
#include "contacts.h"
#include "coverage.h"
#include "ed25519.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "identity.h"
#include "mbedtls/aes.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include "mc_crypto.h"
#include "meshcore/packet.h"
#include "meshcore/payload/advert.h"
#include "meshcore/payload/grp_txt.h"
#include "nodes.h"
#include "radio.h"
#include "settings_nvs.h"
#include "sounds.h"
#include "ui_state.h"

static const char* TAG = "mc_rx";

// Enumerate senders whose 1-byte src_hash (truncated pubkey) matches, one
// candidate per call, so the caller can try each one's key and let the MAC
// decide which sender is real. A 1-byte hash collides about 1/256 per node
// pair, and trusting the first match silently failed DMs and ACKs from the
// loser. *cursor starts at 0: node_list is scanned as 0..MAX_NODES-1 and, when
// include_contacts is set, contacts[] as MAX_NODES.. . Fills out_pub (plus
// optional name/role) for the match and advances *cursor past it. Returns
// false once no further candidate matches.
static bool find_next_sender_by_hash(uint8_t src_hash, bool include_contacts, int* cursor, uint8_t out_pub[32],
                                     char* out_name, size_t name_cap, meshcore_device_role_t* out_role) {
    // Hold node_mutex across BOTH phases: it guards node_list and contacts[],
    // and a concurrent UI-thread contact edit shifts/zeroes contacts[] mid-scan.
    // The lock is released before returning, so the caller's per-candidate
    // ed25519 work runs outside it.
    if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return false;

    while (*cursor < MAX_NODES) {
        int ni = (*cursor)++;
        if (node_list[ni].active && node_list[ni].pub_key[0] == src_hash) {
            memcpy(out_pub, node_list[ni].pub_key, 32);
            if (out_name && name_cap > 0) {
                strncpy(out_name, node_list[ni].name, name_cap - 1);
                out_name[name_cap - 1] = '\0';
            }
            if (out_role) *out_role = node_list[ni].role;
            xSemaphoreGive(node_mutex);
            return true;
        }
    }

    if (include_contacts) {
        while (*cursor < MAX_NODES + contact_count) {
            int ci = (*cursor)++ - MAX_NODES;
            if (contacts[ci].pub_key[0] == src_hash) {
                memcpy(out_pub, contacts[ci].pub_key, 32);
                if (out_name && name_cap > 0) {
                    strncpy(out_name, contacts[ci].alias, name_cap - 1);
                    out_name[name_cap - 1] = '\0';
                }
                if (out_role) *out_role = (meshcore_device_role_t)contacts[ci].role;
                xSemaphoreGive(node_mutex);
                return true;
            }
        }
    }
    xSemaphoreGive(node_mutex);
    return false;
}

// ── GRP_TXT decrypt (private helper used by lora_rx_task) ────────────────────
// Thin wrapper over the host-tested pure implementation in mc_crypto.
static bool decrypt_grp_txt(meshcore_grp_txt_t* grp, const uint8_t* key) {
    return mc_crypto_grp_decrypt(grp, key);
}

// Decrypt a DM payload from a known sender. Derives both ed25519 variants
// of the shared secret and tries 4 HMAC combinations (conv/raw × 16/32-byte
// key) until one matches the on-wire 2-byte MAC. On success, AES-128-ECB
// decrypts the ciphertext into out_plaintext (caller-allocated; must hold
// at least ciphertext-length bytes), sets *out_text_len = ct_len - 5 (the
// timestamp[4] + flags[1] header is included in the buffer), and copies
// the winning secret to out_good_secret so the caller can reuse it for
// PATH_RETURN encryption.
// Thin wrapper over the host-tested pure implementation in mc_crypto.
static bool dm_decrypt(const meshcore_message_t* msg, const uint8_t sender_pub[32], uint8_t* out_plaintext,
                       int* out_text_len, uint8_t out_good_secret[32]) {
    return mc_crypto_dm_decrypt(msg->payload, msg->payload_length, sender_pub, node_prv_key, out_plaintext,
                                out_text_len, out_good_secret);
}

// Build + transmit the PATH_RETURN packet that acknowledges a received DM.
// Layout (createPathReturn approach):
//   inner (AES-ECB, 16 or 32 B): path_len_byte | type=ACK | crc[4] | path_bytes[hops*bph] | zero-pad
//   outer payload:               dst[1] | src[1] | mac[2] | path_cipher[...]
// Inner is encrypted with the shared secret; outer MAC = HMAC-SHA256(secret, cipher)[0..1].
// Subject to the duty-cycle budget — dropped (with a log warning) when air-time would overflow.
static void dm_send_path_return(const meshcore_message_t* msg, uint8_t src_hash, const uint8_t good_secret[32],
                                const uint8_t* plaintext, int text_len, const char* dm_text,
                                const uint8_t sender_pub[32]) {
    (void)text_len;  // dm_text already carries the NUL boundary via plaintext[5+text_len-1]=0

    // 1. ACK CRC = SHA256(timestamp+flags | text | sender_pub)[0..3]
    uint8_t sha_out[4];
    mc_crypto_ack_crc(plaintext, dm_text, strlen(dm_text), sender_pub, sha_out);

    // 2. Build inner block. Reverse incoming hops so the receiver learns the
    // sequence of repeaters back to us. Cap so the inner buffer stays within
    // 2 AES blocks (32 B); longer paths fall back to 0 hops.
    uint8_t ret_bph        = msg->bytes_per_hop ? msg->bytes_per_hop : 1;
    uint8_t ret_hop_count  = (ret_bph > 0) ? (msg->path_length / ret_bph) : 0;
    uint8_t ret_path_bytes = ret_hop_count * ret_bph;
    if (ret_path_bytes > 26) {
        ret_hop_count  = 0;
        ret_path_bytes = 0;
    }

    size_t inner_size = ((6 + (size_t)ret_path_bytes + 15) / 16) * 16;
    if (inner_size < 16) inner_size = 16;
    if (inner_size > 32) inner_size = 32;

    uint8_t inner[32]       = {0};
    uint8_t path_cipher[32] = {0};
    inner[0]                = ((uint8_t)((ret_bph - 1) & 0x03) << 6) | (ret_hop_count & 0x3F);
    inner[1]                = MESHCORE_PAYLOAD_TYPE_ACK;
    inner[2]                = sha_out[0];
    inner[3]                = sha_out[1];
    inner[4]                = sha_out[2];
    inner[5]                = sha_out[3];
    for (uint8_t h = 0; h < ret_hop_count; h++) {
        uint8_t src_hop = ret_hop_count - 1 - h;
        memcpy(&inner[6 + h * ret_bph], &msg->path[src_hop * ret_bph], ret_bph);
    }

    {
        mbedtls_aes_context aes2;
        mbedtls_aes_init(&aes2);
        mbedtls_aes_setkey_enc(&aes2, good_secret, 128);
        for (size_t bi = 0; bi < inner_size; bi += 16) {
            mbedtls_aes_crypt_ecb(&aes2, MBEDTLS_AES_ENCRYPT, &inner[bi], &path_cipher[bi]);
        }
        mbedtls_aes_free(&aes2);
    }

    // 3. Outer MAC + assemble PATH packet (still flood-routed).
    uint8_t path_mac[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), good_secret, 32, path_cipher, inner_size, path_mac);

    meshcore_message_t path_msg = {0};
    path_msg.type               = MESHCORE_PAYLOAD_TYPE_PATH;
    path_msg.route              = MESHCORE_ROUTE_TYPE_FLOOD;
    path_msg.version            = 0;
    path_msg.bytes_per_hop      = path_hash_size;
    path_msg.path_length        = 0;
    path_msg.payload[0]         = src_hash;
    path_msg.payload[1]         = node_pub_key[0];
    path_msg.payload[2]         = path_mac[0];
    path_msg.payload[3]         = path_mac[1];
    memcpy(&path_msg.payload[4], path_cipher, inner_size);
    path_msg.payload_length = 4 + inner_size;
    ESP_LOGI(TAG, "PATH_RETURN: hops=%u bph=%u inner_size=%u", (unsigned)ret_hop_count, (unsigned)ret_bph,
             (unsigned)inner_size);
    radio_tx_message(&path_msg);
}

static void rx_handle_advert(const meshcore_message_t* msg, uint32_t now_ms, const lora_packet_stats_t* stats) {
    meshcore_advert_t advert;
    if (meshcore_advert_deserialize(msg->payload, msg->payload_length, &advert) >= 0) {
        update_node(&advert, now_ms, stats);
    }
}

static void rx_handle_grp_txt(const meshcore_message_t* msg) {
    meshcore_grp_txt_t grp = {0};
    if (meshcore_grp_txt_deserialize(msg->payload, msg->payload_length, &grp) < 0) return;

    // Brute-force over all configured channels. The wire channel_hash is
    // only an optimisation hint — we use the matching channel index to pick
    // the secret, but accept on MAC verify only. If the hash doesn't match
    // anything we still try every key (some senders set hash=0 even when
    // encrypting with a real key).
    int  hit       = channels_find_by_hash(grp.channel_hash);
    int  start     = (hit >= 0) ? hit : 0;
    int  end       = (hit >= 0) ? hit + 1 : channel_count;
    bool decrypted = false;
    for (int ci = start; ci < end; ci++) {
        if (!channels[ci].active) continue;
        meshcore_grp_txt_t attempt = grp;  // structcopy: keep original mac+data for next try
        if (decrypt_grp_txt(&attempt, channels[ci].secret)) {
            ESP_LOGI(TAG, "Channel RX [%s]: %s", channels[ci].name, attempt.decrypted.text);
            // Routes to channel ci's own history; only hits the visible ring
            // if ci is the active channel.
            bool added = ch_add_message_for(ci, attempt.decrypted.text, false);
            if (added) {
                uint8_t bph  = msg->bytes_per_hop ? msg->bytes_per_hop : 1;
                uint8_t hops = (uint8_t)(msg->path_length / bph);
                chat_set_meta_channel(hops);
            }
            if (!(added && current_view == VIEW_CHANNEL)) {
                led_channel_pending = true;
                channel_unread[ci]++;
                update_notification_led();
                sounds_play_channel();
            }
            decrypted = true;
            break;
        }
    }
    if (!decrypted && hit >= 0) {
        // Hash claimed a known channel but MAC didn't verify — fall back to
        // trying everything else. (No sound on this path: it's a recovery
        // case, not a normal channel arrival.)
        for (int ci = 0; ci < channel_count; ci++) {
            if (ci == hit || !channels[ci].active) continue;
            meshcore_grp_txt_t attempt = grp;
            if (decrypt_grp_txt(&attempt, channels[ci].secret)) {
                ESP_LOGI(TAG, "Channel RX [%s] (hash mismatch): %s", channels[ci].name, attempt.decrypted.text);
                bool added = ch_add_message_for(ci, attempt.decrypted.text, false);
                if (added) {
                    uint8_t bph  = msg->bytes_per_hop ? msg->bytes_per_hop : 1;
                    uint8_t hops = (uint8_t)(msg->path_length / bph);
                    chat_set_meta_channel(hops);
                }
                if (!(added && current_view == VIEW_CHANNEL)) {
                    led_channel_pending = true;
                    channel_unread[ci]++;
                    update_notification_led();
                }
                break;
            }
        }
    }
}

static void rx_handle_dm(const meshcore_message_t* msg) {
    if (!identity_is_ready() || msg->payload_length < 6) {
        chat_add_message("DM: not ready/short", false);
        return;
    }
    uint8_t dest_hash = msg->payload[0];
    uint8_t src_hash  = msg->payload[1];
    if (dest_hash != node_pub_key[0]) {
        ESP_LOGD(TAG, "DM not for us (dst=%02X us=%02X)", dest_hash, node_pub_key[0]);
        return;
    }
    if (msg->payload_length < 2 + MESHCORE_CIPHER_MAC_SIZE + 16) {
        chat_add_message("DM: payload too short", false);
        return;
    }

    uint8_t                sender_pub[32]                                 = {0};
    char                   sender_name[MESHCORE_MAX_ADVERT_DATA_SIZE + 1] = {0};
    meshcore_device_role_t sender_role                                    = MESHCORE_DEVICE_ROLE_CHAT_NODE;

    uint8_t plaintext[MESHCORE_MAX_PAYLOAD_SIZE] = {0};
    int     text_len                             = 0;
    uint8_t good_secret[32];

    // The 1-byte src_hash only narrows the field; the per-message MAC is the
    // real disambiguator. Try every node whose pubkey starts with src_hash and
    // let mc_crypto_dm_decrypt decide, so a 1-byte hash collision no longer
    // fails a valid DM (the loser of the collision used to be picked blindly).
    // On success sender_pub/name/role hold the winning candidate.
    bool any_candidate = false;
    bool decrypted     = false;
    int  cursor        = 0;
    while (find_next_sender_by_hash(src_hash, false, &cursor, sender_pub, sender_name, sizeof(sender_name),
                                    &sender_role)) {
        any_candidate = true;
        if (dm_decrypt(msg, sender_pub, plaintext, &text_len, good_secret)) {
            decrypted = true;
            break;
        }
    }
    if (!decrypted) {
        char dbg[48];
        if (!any_candidate) {
            // No node whose hash matches (e.g. missed advert after reboot).
            // Surface the truncated hash and wait for the next advert.
            snprintf(dbg, sizeof(dbg), "[?%02X] DM received (sender unknown)", src_hash);
        } else {
            ESP_LOGW(TAG, "DM HMAC mismatch from %02X — no candidate key verified", src_hash);
            snprintf(dbg, sizeof(dbg), "[?%02X] DM decrypt failed", src_hash);
        }
        chat_add_message(dbg, false);
        return;
    }
    if (text_len <= 0) {
        chat_add_message("DM: no text", false);
        return;
    }

    // plaintext layout: timestamp[4] | flags[1] | text[...]
    plaintext[5 + text_len - 1] = '\0';
    char* dm_text               = (char*)&plaintext[5];

    char display[256];
    if (sender_name[0])
        snprintf(display, sizeof(display), "[%s] %s", sender_name, dm_text);
    else
        snprintf(display, sizeof(display), "[?%02X] %s", src_hash, dm_text);

    ESP_LOGI(TAG, "DM RX: %s", display);
    chat_add_dm(display, false, sender_pub);
    {
        uint8_t bph  = msg->bytes_per_hop ? msg->bytes_per_hop : 1;
        uint8_t hops = (uint8_t)(msg->path_length / bph);
        chat_set_meta_dm(hops);
    }
    // contacts[] is guarded by node_mutex (the UI thread mutates it). These
    // writes run on the RX task, so take the lock around them.
    if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        contact_ensure(sender_pub, sender_name, (uint8_t)sender_role);
        xSemaphoreGive(node_mutex);
    }

    bool viewing_sender = (current_view == VIEW_CHAT && !dm_inbox_mode && dm_target_set &&
                           memcmp(sender_pub, dm_target_pub, MESHCORE_PUB_KEY_SIZE) == 0);
    if (!viewing_sender) {
        led_dm_pending = true;
        if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            contact_mark_unread(sender_pub);
            xSemaphoreGive(node_mutex);
        }
        update_notification_led();
        sounds_play_dm();
    }

    dm_send_path_return(msg, src_hash, good_secret, plaintext, text_len, dm_text, sender_pub);
}

static void rx_handle_path(const meshcore_message_t* msg) {
    // Incoming PATH_RETURN. We only care about it when it carries an ACK
    // for one of our outgoing DMs — match by ack_crc[4] inside the encrypted
    // inner block.
    if (msg->payload_length < 4 + 16) return;
    uint8_t dest_hash = msg->payload[0];
    uint8_t src_hash  = msg->payload[1];
    if (dest_hash != node_pub_key[0]) return;

    size_t ct_len = msg->payload_length - 4;
    if (ct_len > 32) ct_len = 32;
    ct_len = (ct_len / 16) * 16;
    if (ct_len < 16) return;

    // src_hash is a 1-byte hint; a collision would derive the wrong shared
    // secret. Try each candidate sender (nodes + contacts) and let the
    // decrypted ACK marker decide, mirroring the DM-RX candidate loop. Derive
    // the shared secret per candidate (ed25519 conv variant; PATH never used
    // the raw fallback), decrypt one or two AES blocks and see if inner[1] =
    // ACK and inner[2..5] matches a recent own DM's CRC.
    uint8_t sender_pub[32] = {0};
    int     cursor         = 0;
    while (find_next_sender_by_hash(src_hash, true, &cursor, sender_pub, NULL, 0, NULL)) {
        uint8_t secret[32];
        ed25519_key_exchange(secret, sender_pub, node_prv_key);

        uint8_t inner[32]      = {0};
        uint8_t ciphertext[32] = {0};
        memcpy(ciphertext, &msg->payload[4], ct_len);
        mbedtls_aes_context aes_ctx;
        mbedtls_aes_init(&aes_ctx);
        mbedtls_aes_setkey_dec(&aes_ctx, secret, 128);
        for (size_t bi = 0; bi + 16 <= ct_len; bi += 16) {
            mbedtls_aes_crypt_ecb(&aes_ctx, MBEDTLS_AES_DECRYPT, ciphertext + bi, inner + bi);
        }
        mbedtls_aes_free(&aes_ctx);

        // inner[0] = path_len_byte, inner[1] = payload type. A wrong key yields
        // garbage that almost never reads as ACK, so try the next candidate.
        if (inner[1] != MESHCORE_PAYLOAD_TYPE_ACK) continue;
        uint8_t ack_crc[4] = {inner[2], inner[3], inner[4], inner[5]};
        if (chat_mark_ack_by_crc(ack_crc)) {
            ESP_LOGI(TAG, "ACK matched: %02X%02X%02X%02X from %02X", ack_crc[0], ack_crc[1], ack_crc[2], ack_crc[3],
                     src_hash);
        }
        // Also offer the ACK to the coverage-test ping controller, which tracks
        // its pings outside the chat ring.
        coverage_note_ack(ack_crc);
        return;  // this candidate produced a valid ACK block; done
    }
}

static void mc_rx_dispatch(const meshcore_message_t* msg, const radio_rx_meta_t* meta) {
    switch (msg->type) {
        case MESHCORE_PAYLOAD_TYPE_ADVERT:
            rx_handle_advert(msg, meta->now_ms, &meta->stats);
            break;
        case MESHCORE_PAYLOAD_TYPE_GRP_TXT:
            rx_handle_grp_txt(msg);
            break;
        case MESHCORE_PAYLOAD_TYPE_TXT_MSG:
            rx_handle_dm(msg);
            break;
        case MESHCORE_PAYLOAD_TYPE_PATH:
            rx_handle_path(msg);
            break;
        default:
            break;
    }
}

void mc_rx_init(void) {
    radio_set_rx_sink(mc_rx_dispatch);
}

// ── TX composers (moved out of radio.c; radio is transport-only) ─────────────
// c6_available + last_advert_ms are radio/main globals (radio.h / main.c).
extern bool c6_available;

// Background-task payload: pub-key hashes pre-computed under a brief node_mutex
// hold, plus the path-hash byte-count, passed to send_advert_internal one
// packet at a time.
typedef struct {
    int     n;
    uint8_t bph;
    uint8_t hashes[];
} adv_direct_args_t;

static void send_advert_internal(bool direct_route, const uint8_t* dst_hash, uint8_t bph) {
    if (!c6_available || !identity_is_ready()) return;

    meshcore_advert_t advert = {0};
    memcpy(advert.pub_key, node_pub_key, MESHCORE_PUB_KEY_SIZE);
    // UNIX epoch — matches upstream MeshCore convention and what the other
    // payload paths in this file use. Earlier versions accidentally used
    // xTaskGetTickCount-derived uptime here, which other clients rejected
    // because the timestamp looked decades old (~15 since boot).
    advert.timestamp = (uint32_t)time(NULL);
    advert.role      = lora_role;

    const char* adv_src =
        lora_advert_name[0] ? lora_advert_name : ((owner_name[0] && owner_name[0] != '(') ? owner_name : NULL);
    if (adv_src) {
        strncpy(advert.name, adv_src, MESHCORE_MAX_NAME_SIZE);
        advert.name_valid = true;
    }

    if (gps_position_valid) {
        advert.position_valid = true;
        advert.position_lat   = gps_lat_e6;
        advert.position_lon   = gps_lon_e6;
    }

    uint8_t payload[MESHCORE_MAX_PAYLOAD_SIZE];
    uint8_t payload_len = 0;
    if (meshcore_advert_serialize(&advert, payload, &payload_len) < 0) return;

    uint8_t to_sign[MESHCORE_MAX_PAYLOAD_SIZE];
    uint8_t to_sign_len = meshcore_advert_signable_bytes(payload, payload_len, to_sign);

    ed25519_sign(advert.signature, to_sign, to_sign_len, node_pub_key, node_prv_key);

    if (meshcore_advert_serialize(&advert, payload, &payload_len) < 0) return;

    meshcore_message_t msg = {0};
    msg.type               = MESHCORE_PAYLOAD_TYPE_ADVERT;
    msg.route              = direct_route ? MESHCORE_ROUTE_TYPE_DIRECT : MESHCORE_ROUTE_TYPE_FLOOD;
    msg.bytes_per_hop      = path_hash_size;
    if (direct_route && dst_hash && bph > 0) {
        // Populate path with dst hash so upstream receivers match this as
        // a direct-routed packet addressed to them (or to a peer they
        // route for).
        uint8_t cap = bph > MESHCORE_MAX_PATH_SIZE ? MESHCORE_MAX_PATH_SIZE : bph;
        memcpy(msg.path, dst_hash, cap);
        msg.path_length = cap;
    }
    msg.payload_length = payload_len;
    memcpy(msg.payload, payload, payload_len);
    if (radio_tx_message(&msg)) {
        last_advert_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        ESP_LOGI(TAG, "ADVERT sent (%s) pub=%02X%02X%02X%02X", advert.name_valid ? advert.name : "(no name)",
                 node_pub_key[0], node_pub_key[1], node_pub_key[2], node_pub_key[3]);
    }
}

void send_advert(void) {
    send_advert_internal(false, NULL, 0);
}

static void send_advert_direct_task(void* arg) {
    adv_direct_args_t* a = (adv_direct_args_t*)arg;
    if (a->n == 0) {
        send_advert_internal(true, NULL, 0);
    } else {
        for (int i = 0; i < a->n; i++) {
            send_advert_internal(true, a->hashes + i * 32, a->bph);
            // 250 ms gap — a bit more generous than the flood path's 150 ms
            // so 16 sequential TXs stay under ~1 % of the 1-hour duty budget.
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }
    free(a);
    vTaskDelete(NULL);
}

// DIRECT advert: stock MeshCore receivers drop direct packets with no
// destination match, so we send one direct-route advert per *contact*
// (user-favorited / DM'd peers, MAX_CONTACTS = 16), addressed by
// sha256(pub_key)[0..bph-1]. Iterating every background-discovered node
// (up to 200) wastes airtime — most of those are out-of-route repeaters
// the user has no relationship with. Limiting to contacts caps the worst
// case at ~16 × (TX + 200 ms gap) ≈ 5-6 seconds of activity and a few
// tenths of a percent duty-cycle hit.
void send_advert_direct(void) {
    if (!c6_available || !identity_is_ready()) return;
    uint8_t bph = path_hash_size ? path_hash_size : 1;

    // Worst case MAX_CONTACTS × 32-byte hashes = 512 B. Allocate from heap
    // so the task can run on a modest stack.
    size_t             cap = sizeof(adv_direct_args_t) + (size_t)MAX_CONTACTS * 32u;
    adv_direct_args_t* a   = (adv_direct_args_t*)malloc(cap);
    if (!a) {
        // OOM — fall back to the single no-dst direct so the user still
        // gets feedback (toast is set by the caller after this returns).
        send_advert_internal(true, NULL, 0);
        return;
    }
    a->n   = 0;
    a->bph = bph;
    if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int i = 0; i < contact_count && a->n < MAX_CONTACTS; i++) {
            mbedtls_sha256(contacts[i].pub_key, MESHCORE_PUB_KEY_SIZE, a->hashes + a->n * 32, 0);
            a->n++;
        }
        xSemaphoreGive(node_mutex);
    }
    BaseType_t r = xTaskCreate(send_advert_direct_task, "adv_direct", 4096, a, 3, NULL);
    if (r != pdPASS) {
        free(a);
        send_advert_internal(true, NULL, 0);
    }
}

// ── ADVERT broadcast task ────────────────────────────────────────────────────
// Honours both flood + direct interval settings. Value 0 on either = that
// schedule is off. Manual sends via the Advert UI keep working in both
// cases. Re-reads the interval each second so retuning in Settings takes
// effect immediately.
static void advert_task(void* arg) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    uint32_t flood_elapsed_ms  = 0;
    uint32_t direct_elapsed_ms = 0;
    while (1) {
        uint16_t flood_int  = flood_advert_interval_s;
        uint16_t direct_int = direct_advert_interval_s;

        if (flood_int > 0 && flood_elapsed_ms >= (uint32_t)flood_int * 1000u) {
            send_advert();
            flood_elapsed_ms = 0;
        }
        if (direct_int > 0 && direct_elapsed_ms >= (uint32_t)direct_int * 1000u) {
            send_advert_direct();
            direct_elapsed_ms = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
        flood_elapsed_ms  += 1000;
        direct_elapsed_ms += 1000;

        // Reset elapsed counters when their schedule is off so re-enabling
        // doesn't trigger an immediate emit.
        if (flood_int == 0) flood_elapsed_ms = 0;
        if (direct_int == 0) direct_elapsed_ms = 0;
    }
}

// ── DM TX (TXT_MSG) ──────────────────────────────────────────────────────────
bool send_dm_message(const char* text, const uint8_t* target_pub, uint8_t ack_crc_out[4]) {
    if (!c6_available || !identity_is_ready()) return false;

    uint32_t ts                               = (uint32_t)time(NULL);
    uint8_t  plain[MESHCORE_MAX_PAYLOAD_SIZE] = {0};
    size_t   text_len                         = strlen(text);
    size_t   plain_len                        = 5 + text_len;
    size_t   padded                           = ((plain_len + 15) / 16) * 16;
    if (padded > MESHCORE_MAX_PAYLOAD_SIZE - 4) return false;

    memcpy(plain, &ts, 4);
    plain[4] = 0;  // flags
    memcpy(&plain[5], text, text_len);

    // ACK CRC: the receiver computes SHA256(plain[0..5] || text || OUR pubkey)[0:4]
    // to bind the ACK to this specific message. Mirror that here so the caller
    // can store it on the chat_msg and detect the matching PATH_RETURN later.
    if (ack_crc_out) {
        mc_crypto_ack_crc(plain, text, text_len, node_pub_key, ack_crc_out);
    }

    uint8_t cipher[MESHCORE_MAX_PAYLOAD_SIZE] = {0};
    uint8_t mac[32];
    mc_crypto_dm_encrypt(target_pub, node_prv_key, plain, padded, cipher, mac);

    meshcore_message_t msg = {0};
    msg.type               = MESHCORE_PAYLOAD_TYPE_TXT_MSG;
    msg.route              = MESHCORE_ROUTE_TYPE_FLOOD;
    msg.version            = 0;
    msg.bytes_per_hop      = path_hash_size;
    msg.path_length        = 0;
    msg.payload[0]         = target_pub[0];    // dest hash
    msg.payload[1]         = node_pub_key[0];  // src  hash
    msg.payload[2]         = mac[0];
    msg.payload[3]         = mac[1];
    memcpy(&msg.payload[4], cipher, padded);
    msg.payload_length = (uint8_t)(4 + padded);
    return radio_tx_message(&msg);
}

// ── Coverage test: ping a repeater N times and record reachability ───────────
// A "ping" is just a DM to the repeater's pubkey; the matching PATH_RETURN ACK
// is detected via the coverage matcher (coverage_note_ack in rx_handle_path),
// not the chat ring, so DM history stays clean. Runs on its own task so the UI
// never blocks across the 3x10 s schedule.
typedef struct {
    uint8_t pub[32];
    char    name[MESHCORE_MAX_NAME_SIZE + 1];
    int32_t lat_e6;
    int32_t lon_e6;
    bool    gps_valid;
} coverage_ping_arg_t;

static void coverage_ping_task(void* arg) {
    coverage_ping_arg_t* a = (coverage_ping_arg_t*)arg;
    coverage_set_testing(a->pub);

    for (int i = 0; i < COVERAGE_PINGS; i++) {
        uint8_t    crc[4] = {0};
        TickType_t t0     = xTaskGetTickCount();
        bool       ack    = false;
        uint32_t   rtt_ms = 0;

        if (send_dm_message("ping", a->pub, crc)) {
            coverage_arm_ack(crc);
            // Poll for the PATH_RETURN ACK up to ~8 s.
            for (int waited = 0; waited < 8000; waited += 100) {
                if (coverage_take_ack()) {
                    ack    = true;
                    rtt_ms = (uint32_t)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS);
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }

        coverage_record(a->pub, ack);
        coverage_log(a->pub, a->name, a->lat_e6, a->lon_e6, a->gps_valid, i, ack, rtt_ms);

        if (i < COVERAGE_PINGS - 1) vTaskDelay(pdMS_TO_TICKS(10000));
    }

    coverage_set_busy(false);
    free(a);
    vTaskDelete(NULL);
}

void coverage_ping_start(const uint8_t* pub, const char* name, int32_t lat_e6, int32_t lon_e6, bool gps_valid) {
    if (pub == NULL || coverage_busy()) return;
    coverage_ping_arg_t* a = calloc(1, sizeof(*a));
    if (a == NULL) return;
    memcpy(a->pub, pub, 32);
    if (name) strncpy(a->name, name, sizeof(a->name) - 1);
    a->lat_e6    = lat_e6;
    a->lon_e6    = lon_e6;
    a->gps_valid = gps_valid;

    coverage_set_busy(true);  // reflect immediately so the UI can't double-start
    if (xTaskCreate(coverage_ping_task, "cov_ping", 4096, a, 3, NULL) != pdPASS) {
        coverage_set_busy(false);
        free(a);
    }
}

// ── Public-channel TX (GRP_TXT) ──────────────────────────────────────────────
bool send_chat_message(const char* text) {
    if (!c6_available) return false;

    uint32_t ts                               = (uint32_t)time(NULL);
    uint8_t  plain[MESHCORE_MAX_PAYLOAD_SIZE] = {0};
    size_t   text_len                         = strlen(text);

    // Prefix with name so other clients can attribute the message.
    const char* name_src =
        lora_advert_name[0] ? lora_advert_name : ((owner_name[0] && owner_name[0] != '(') ? owner_name : NULL);
    char prefixed[MAX_MSG_TEXT];
    if (name_src) {
        snprintf(prefixed, sizeof(prefixed), "%s: %s", name_src, text);
    } else {
        snprintf(prefixed, sizeof(prefixed), "%s", text);
    }
    text_len = strlen(prefixed);

    size_t plain_len = 4 + 1 + text_len;
    size_t padded    = ((plain_len + 15) / 16) * 16;
    if (padded > sizeof(plain)) return false;

    memcpy(plain, &ts, 4);
    plain[4] = 0;  // text_type = normal
    memcpy(&plain[5], prefixed, text_len);

    // Pick the active channel's key + hash; fall back to Public (idx 0) if
    // active_channel_idx is somehow out of range.
    int ch_idx = (active_channel_idx >= 0 && active_channel_idx < channel_count && channels[active_channel_idx].active)
                     ? active_channel_idx
                     : 0;
    const uint8_t* ch_secret = channels[ch_idx].secret;
    uint8_t        ch_hash   = channels[ch_idx].hash;

    uint8_t cipher[MESHCORE_MAX_PAYLOAD_SIZE] = {0};
    uint8_t mac[32];
    mc_crypto_grp_encrypt(ch_secret, plain, padded, cipher, mac);

    meshcore_grp_txt_t grp = {0};
    grp.channel_hash       = ch_hash;
    memcpy(grp.mac, mac, MESHCORE_CIPHER_MAC_SIZE);
    grp.data_length = (uint8_t)padded;
    memcpy(grp.data, cipher, padded);

    uint8_t payload[MESHCORE_MAX_PAYLOAD_SIZE];
    uint8_t payload_len = 0;
    if (meshcore_grp_txt_serialize(&grp, payload, &payload_len) < 0) return false;

    meshcore_message_t msg = {0};
    msg.type               = MESHCORE_PAYLOAD_TYPE_GRP_TXT;
    msg.route              = MESHCORE_ROUTE_TYPE_FLOOD;
    msg.version            = 0;
    msg.bytes_per_hop      = path_hash_size;
    msg.path_length        = 0;
    msg.payload_length     = payload_len;
    memcpy(msg.payload, payload, payload_len);
    bool ok = radio_tx_message(&msg);
    if (ok) ESP_LOGI(TAG, "Chat sent: %s", prefixed);
    return ok;
}

void mc_rx_start_advert_task(void) {
    xTaskCreate(advert_task, "lora_advert", 6144, NULL, 4, NULL);
}
