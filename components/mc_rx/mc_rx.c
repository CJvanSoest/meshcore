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
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "mbedtls/aes.h"
#include "mbedtls/md.h"

#include "ed25519.h"
#include "meshcore/packet.h"
#include "meshcore/payload/advert.h"
#include "meshcore/payload/grp_txt.h"

#include "mc_crypto.h"
#include "radio.h"

#include "app_config.h"
#include "ui_state.h"
#include "channels.h"
#include "chat.h"
#include "contacts.h"
#include "identity.h"
#include "nodes.h"
#include "settings_nvs.h"
#include "sounds.h"

static const char *TAG = "mc_rx";

// Locate a sender by 1-byte src_hash (truncated pubkey). Always searches
// node_list first; if include_contacts is true and node_list misses, falls
// back to the contacts[] table. out_name + out_role may be NULL when the
// caller (PATH handler) only needs the pubkey.
static bool find_sender_by_hash(uint8_t src_hash, bool include_contacts,
                                uint8_t out_pub[32],
                                char *out_name, size_t name_cap,
                                meshcore_device_role_t *out_role) {
    bool found = false;
    if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        for (int ni = 0; ni < MAX_NODES; ni++) {
            if (node_list[ni].active && node_list[ni].pub_key[0] == src_hash) {
                memcpy(out_pub, node_list[ni].pub_key, 32);
                if (out_name && name_cap > 0) {
                    strncpy(out_name, node_list[ni].name, name_cap - 1);
                    out_name[name_cap - 1] = '\0';
                }
                if (out_role) *out_role = node_list[ni].role;
                found = true;
                break;
            }
        }
        xSemaphoreGive(node_mutex);
    }
    if (!found && include_contacts) {
        for (int ci = 0; ci < contact_count; ci++) {
            if (contacts[ci].pub_key[0] == src_hash) {
                memcpy(out_pub, contacts[ci].pub_key, 32);
                if (out_name && name_cap > 0) {
                    strncpy(out_name, contacts[ci].alias, name_cap - 1);
                    out_name[name_cap - 1] = '\0';
                }
                if (out_role) *out_role = (meshcore_device_role_t)contacts[ci].role;
                found = true;
                break;
            }
        }
    }
    return found;
}

// ── GRP_TXT decrypt (private helper used by lora_rx_task) ────────────────────
// Thin wrapper over the host-tested pure implementation in mc_crypto.
static bool decrypt_grp_txt(meshcore_grp_txt_t *grp, const uint8_t *key) {
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
static bool dm_decrypt(const meshcore_message_t *msg, const uint8_t sender_pub[32],
                       uint8_t *out_plaintext, int *out_text_len,
                       uint8_t out_good_secret[32]) {
    return mc_crypto_dm_decrypt(msg->payload, msg->payload_length, sender_pub,
                                node_prv_key, out_plaintext, out_text_len, out_good_secret);
}

// Build + transmit the PATH_RETURN packet that acknowledges a received DM.
// Layout (createPathReturn approach):
//   inner (AES-ECB, 16 or 32 B): path_len_byte | type=ACK | crc[4] | path_bytes[hops*bph] | zero-pad
//   outer payload:               dst[1] | src[1] | mac[2] | path_cipher[...]
// Inner is encrypted with the shared secret; outer MAC = HMAC-SHA256(secret, cipher)[0..1].
// Subject to the duty-cycle budget — dropped (with a log warning) when air-time would overflow.
static void dm_send_path_return(const meshcore_message_t *msg, uint8_t src_hash,
                                const uint8_t good_secret[32],
                                const uint8_t *plaintext, int text_len,
                                const char *dm_text,
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
    inner[0] = ((uint8_t)((ret_bph - 1) & 0x03) << 6) | (ret_hop_count & 0x3F);
    inner[1] = MESHCORE_PAYLOAD_TYPE_ACK;
    inner[2] = sha_out[0];
    inner[3] = sha_out[1];
    inner[4] = sha_out[2];
    inner[5] = sha_out[3];
    for (uint8_t h = 0; h < ret_hop_count; h++) {
        uint8_t src_hop = ret_hop_count - 1 - h;
        memcpy(&inner[6 + h * ret_bph], &msg->path[src_hop * ret_bph], ret_bph);
    }

    {
        mbedtls_aes_context aes2;
        mbedtls_aes_init(&aes2);
        mbedtls_aes_setkey_enc(&aes2, good_secret, 128);
        for (size_t bi = 0; bi < inner_size; bi += 16) {
            mbedtls_aes_crypt_ecb(&aes2, MBEDTLS_AES_ENCRYPT,
                                  &inner[bi], &path_cipher[bi]);
        }
        mbedtls_aes_free(&aes2);
    }

    // 3. Outer MAC + assemble PATH packet (still flood-routed).
    uint8_t path_mac[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                    good_secret, 32, path_cipher, inner_size, path_mac);

    meshcore_message_t path_msg = {0};
    path_msg.type           = MESHCORE_PAYLOAD_TYPE_PATH;
    path_msg.route          = MESHCORE_ROUTE_TYPE_FLOOD;
    path_msg.version        = 0;
    path_msg.bytes_per_hop  = path_hash_size;
    path_msg.path_length    = 0;
    path_msg.payload[0]     = src_hash;
    path_msg.payload[1]     = node_pub_key[0];
    path_msg.payload[2]     = path_mac[0];
    path_msg.payload[3]     = path_mac[1];
    memcpy(&path_msg.payload[4], path_cipher, inner_size);
    path_msg.payload_length = 4 + inner_size;
    ESP_LOGI(TAG, "PATH_RETURN: hops=%u bph=%u inner_size=%u",
             (unsigned)ret_hop_count, (unsigned)ret_bph, (unsigned)inner_size);
    radio_tx_message(&path_msg);
}

static void rx_handle_advert(const meshcore_message_t *msg, uint32_t now_ms,
                              const lora_packet_stats_t *stats) {
    meshcore_advert_t advert;
    if (meshcore_advert_deserialize(msg->payload, msg->payload_length, &advert) >= 0) {
        update_node(&advert, now_ms, stats);
    }
}

static void rx_handle_grp_txt(const meshcore_message_t *msg) {
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
            ESP_LOGI(TAG, "Channel RX [%s]: %s",
                     channels[ci].name, attempt.decrypted.text);
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
                ESP_LOGI(TAG, "Channel RX [%s] (hash mismatch): %s",
                         channels[ci].name, attempt.decrypted.text);
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

static void rx_handle_dm(const meshcore_message_t *msg) {
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

    uint8_t sender_pub[32] = {0};
    char    sender_name[MESHCORE_MAX_ADVERT_DATA_SIZE + 1] = {0};
    meshcore_device_role_t sender_role = MESHCORE_DEVICE_ROLE_CHAT_NODE;
    if (!find_sender_by_hash(src_hash, false, sender_pub,
                             sender_name, sizeof(sender_name), &sender_role)) {
        // Sender pubkey unknown (e.g. missed advert after reboot). Can't
        // decrypt without pubkey — surface the truncated hash and wait for
        // the next advert.
        char unknown_msg[48];
        snprintf(unknown_msg, sizeof(unknown_msg),
                 "[?%02X] DM received (sender unknown)", src_hash);
        chat_add_message(unknown_msg, false);
        return;
    }

    uint8_t plaintext[MESHCORE_MAX_PAYLOAD_SIZE] = {0};
    int     text_len  = 0;
    uint8_t good_secret[32];
    if (!dm_decrypt(msg, sender_pub, plaintext, &text_len, good_secret)) {
        ESP_LOGW(TAG, "DM HMAC mismatch from %02X — wrong key or unsupported variant", src_hash);
        char dbg[48];
        snprintf(dbg, sizeof(dbg), "[?%02X] DM decrypt failed", src_hash);
        chat_add_message(dbg, false);
        return;
    }
    if (text_len <= 0) {
        chat_add_message("DM: no text", false);
        return;
    }

    // plaintext layout: timestamp[4] | flags[1] | text[...]
    plaintext[5 + text_len - 1] = '\0';
    char *dm_text = (char *)&plaintext[5];

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
    contact_ensure(sender_pub, sender_name, (uint8_t)sender_role);

    bool viewing_sender = (current_view == VIEW_CHAT && !dm_inbox_mode &&
                           dm_target_set &&
                           memcmp(sender_pub, dm_target_pub, MESHCORE_PUB_KEY_SIZE) == 0);
    if (!viewing_sender) {
        led_dm_pending = true;
        contact_mark_unread(sender_pub);
        update_notification_led();
        sounds_play_dm();
    }

    dm_send_path_return(msg, src_hash, good_secret, plaintext, text_len, dm_text, sender_pub);
}

static void rx_handle_path(const meshcore_message_t *msg) {
    // Incoming PATH_RETURN. We only care about it when it carries an ACK
    // for one of our outgoing DMs — match by ack_crc[4] inside the encrypted
    // inner block.
    if (msg->payload_length < 4 + 16) return;
    uint8_t dest_hash = msg->payload[0];
    uint8_t src_hash  = msg->payload[1];
    if (dest_hash != node_pub_key[0]) return;

    uint8_t sender_pub[32] = {0};
    if (!find_sender_by_hash(src_hash, true, sender_pub, NULL, 0, NULL)) return;

    // Derive shared secret (only ed25519 conv variant; PATH never used the
    // raw fallback in the original flow). Decrypt one or two AES blocks and
    // see if inner[1] = ACK and inner[2..5] matches a recent own DM's CRC.
    uint8_t secret[32];
    ed25519_key_exchange(secret, sender_pub, node_prv_key);

    size_t ct_len = msg->payload_length - 4;
    if (ct_len > 32) ct_len = 32;
    ct_len = (ct_len / 16) * 16;
    if (ct_len < 16) return;

    uint8_t inner[32]      = {0};
    uint8_t ciphertext[32] = {0};
    memcpy(ciphertext, &msg->payload[4], ct_len);
    mbedtls_aes_context aes_ctx;
    mbedtls_aes_init(&aes_ctx);
    mbedtls_aes_setkey_dec(&aes_ctx, secret, 128);
    for (size_t bi = 0; bi + 16 <= ct_len; bi += 16) {
        mbedtls_aes_crypt_ecb(&aes_ctx, MBEDTLS_AES_DECRYPT,
                              ciphertext + bi, inner + bi);
    }
    mbedtls_aes_free(&aes_ctx);

    // inner[0] = path_len_byte, inner[1] = payload type.
    if (inner[1] != MESHCORE_PAYLOAD_TYPE_ACK) return;
    uint8_t ack_crc[4] = { inner[2], inner[3], inner[4], inner[5] };
    if (chat_mark_ack_by_crc(ack_crc)) {
        ESP_LOGI(TAG, "ACK matched: %02X%02X%02X%02X from %02X",
                 ack_crc[0], ack_crc[1], ack_crc[2], ack_crc[3], src_hash);
    }
}

static void mc_rx_dispatch(const meshcore_message_t *msg, const radio_rx_meta_t *meta) {
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

void mc_rx_init(void) { radio_set_rx_sink(mc_rx_dispatch); }
