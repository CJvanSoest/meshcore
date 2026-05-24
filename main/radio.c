// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "radio.h"

#include <string.h>
#include <time.h>

#include "freertos/task.h"

#include "esp_log.h"
#include "mbedtls/aes.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"

#include "ed25519.h"
#include "meshcore/packet.h"
#include "meshcore/payload/advert.h"
#include "meshcore/payload/grp_txt.h"

#include "app_config.h"
#include "chat.h"
#include "contacts.h"
#include "identity.h"
#include "nodes.h"
#include "settings_nvs.h"

static const char *TAG = "radio";

// c6_available lives in main.c (set during boot once the C6 responds).
extern bool c6_available;

// ── State ────────────────────────────────────────────────────────────────────
rx_entry_t        rx_buf[RX_BUF_SIZE];
int               rx_head    = 0;
int               rx_count   = 0;
SemaphoreHandle_t rx_mutex   = NULL;
bool              lora_rx_ok = false;

volatile int8_t   last_rx_rssi_dbm        = 0;
volatile int8_t   last_rx_snr_db_x4       = 0;
volatile int8_t   last_rx_signal_rssi_dbm = 0;
volatile uint32_t last_rx_stats_ms        = 0;
volatile bool     last_rx_stats_valid     = false;

volatile int8_t   noise_floor_dbm       = 0;
volatile bool     noise_floor_valid     = false;
volatile bool     noise_floor_supported = true;

uint32_t last_advert_ms = 0;

// ── ADVERT TX ────────────────────────────────────────────────────────────────
void send_advert(void) {
    if (!c6_available || !identity_is_ready()) return;

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

    uint8_t payload[MESHCORE_MAX_PAYLOAD_SIZE];
    uint8_t payload_len = 0;
    if (meshcore_advert_serialize(&advert, payload, &payload_len) < 0) return;

    // Signature covers pub_key + timestamp + flags + name (everything except sig).
    // Layout: pub_key[32] | timestamp[4] | sig[64] | flags[1] | name
    uint8_t to_sign[MESHCORE_MAX_PAYLOAD_SIZE];
    uint8_t to_sign_len = 0;
    memcpy(to_sign, payload, MESHCORE_PUB_KEY_SIZE + 4);
    to_sign_len = MESHCORE_PUB_KEY_SIZE + 4;
    uint8_t after_sig_offset = MESHCORE_PUB_KEY_SIZE + 4 + MESHCORE_SIGNATURE_SIZE;
    if (payload_len > after_sig_offset) {
        memcpy(to_sign + to_sign_len, payload + after_sig_offset, payload_len - after_sig_offset);
        to_sign_len += payload_len - after_sig_offset;
    }

    ed25519_sign(advert.signature, to_sign, to_sign_len, node_pub_key, node_prv_key);

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

// ── GRP_TXT decrypt (private helper used by lora_rx_task) ────────────────────
static bool decrypt_grp_txt(meshcore_grp_txt_t *grp, const uint8_t *key) {
    uint8_t mac[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                    key, MESHCORE_CIPHER_KEY_SIZE,
                    grp->data, grp->data_length,
                    mac);
    if (memcmp(mac, grp->mac, MESHCORE_CIPHER_MAC_SIZE) != 0) return false;

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

    // Parse: timestamp(4) | text_type(1) | text
    if (grp->decrypted.data_length < 5) return false;
    memcpy(&grp->decrypted.timestamp, grp->decrypted.data, 4);
    grp->decrypted.text_type = grp->decrypted.data[4];
    grp->decrypted.data[grp->decrypted.data_length - 1] = '\0';
    grp->decrypted.text = (char *)&grp->decrypted.data[5];
    return true;
}

// ── DM TX (TXT_MSG) ──────────────────────────────────────────────────────────
bool send_dm_message(const char *text, const uint8_t *target_pub) {
    if (!c6_available || !identity_is_ready()) return false;

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
    msg.payload[1]     = node_pub_key[0];   // src  hash
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

// ── Public-channel TX (GRP_TXT) ──────────────────────────────────────────────
bool send_chat_message(const char *text) {
    if (!c6_available) return false;

    uint32_t ts = (uint32_t)time(NULL);
    uint8_t  plain[MESHCORE_MAX_PAYLOAD_SIZE] = {0};
    size_t   text_len = strlen(text);

    // Prefix with name so other clients can attribute the message.
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
    size_t padded    = ((plain_len + 15) / 16) * 16;
    if (padded > sizeof(plain)) return false;

    memcpy(plain, &ts, 4);
    plain[4] = 0;  // text_type = normal
    memcpy(&plain[5], prefixed, text_len);

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

    uint8_t mac[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                    PUBLIC_CHANNEL_KEY, MESHCORE_CIPHER_KEY_SIZE,
                    cipher, (uint16_t)padded, mac);

    meshcore_grp_txt_t grp = {0};
    grp.channel_hash = channel_hash;
    memcpy(grp.mac, mac, MESHCORE_CIPHER_MAC_SIZE);
    grp.data_length = (uint8_t)padded;
    memcpy(grp.data, cipher, padded);

    uint8_t payload[MESHCORE_MAX_PAYLOAD_SIZE];
    uint8_t payload_len = 0;
    if (meshcore_grp_txt_serialize(&grp, payload, &payload_len) < 0) return false;

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

// ── ADVERT broadcast task ────────────────────────────────────────────────────
static void advert_task(void *arg) {
    // Initial delay so the radio is fully up before first advert.
    vTaskDelay(pdMS_TO_TICKS(5000));
    while (1) {
        send_advert();
        uint32_t ms = (uint32_t)advert_interval_s * 1000u;
        if (ms < 5000u) ms = 5000u;
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
}

// ── RX dedup (drop flood retransmits) ────────────────────────────────────────
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

// ── Noise floor poll task ────────────────────────────────────────────────────
static void noise_floor_task(void *arg) {
    ESP_LOGI(TAG, "Noise floor task started");
    while (1) {
        // 60s — 5s caused RX-decode failures (see 2026-05-23 diagnosis).
        vTaskDelay(pdMS_TO_TICKS(60000));
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

            if (xSemaphoreTake(rx_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                rx_buf[rx_head].pkt          = pkt;
                rx_buf[rx_head].timestamp_ms = now_ms;
                rx_head  = (rx_head + 1) % RX_BUF_SIZE;
                if (rx_count < RX_BUF_SIZE) rx_count++;
                xSemaphoreGive(rx_mutex);
            }

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
                    // Use do{}while(0) so 'break' exits this block, not the rx while(1).
                    do {
                        char dbg[48];
                        if (!identity_is_ready() || mc_msg.payload_length < 6) {
                            chat_add_message("DM: not ready/short", false); break;
                        }
                        uint8_t dest_hash = mc_msg.payload[0];
                        uint8_t src_hash  = mc_msg.payload[1];

                        if (dest_hash != node_pub_key[0]) {
                            ESP_LOGD(TAG, "DM not for us (dst=%02X us=%02X)", dest_hash, node_pub_key[0]);
                            break;
                        }

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

                        // ECDH shared secret — try both with and without Edwards→Montgomery.
                        uint8_t secret[32];
                        ed25519_key_exchange(secret, sender_pub, node_prv_key);

                        uint8_t secret_raw[32];
                        ed25519_key_exchange_raw(secret_raw, sender_pub, node_prv_key);

                        const uint8_t *mac_ct     = &mc_msg.payload[2];
                        int            mac_ct_len = mc_msg.payload_length - 2;
                        if (mac_ct_len < MESHCORE_CIPHER_MAC_SIZE + 16) {
                            chat_add_message("DM: payload too short", false); break;
                        }

                        const uint8_t *ciphertext = mac_ct + MESHCORE_CIPHER_MAC_SIZE;
                        int            ct_len     = mac_ct_len - MESHCORE_CIPHER_MAC_SIZE;

                        uint8_t hmac_conv[32], hmac_raw[32];
                        mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                                        secret, 32, ciphertext, ct_len, hmac_conv);
                        mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                                        secret_raw, 32, ciphertext, ct_len, hmac_raw);
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

                        // AES-128-ECB decrypt (key = good_secret[0..15]).
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

                        // Send PATH_RETURN with embedded ACK (createPathReturn approach).
                        // For FLOOD DMs, MeshCore sends PAYLOAD_TYPE_PATH (0x08), not a bare ACK.
                        // Inner data (16 bytes, AES-ECB encrypted):
                        //   path_len=0 | PAYLOAD_TYPE_ACK | ack_crc[4] | padding[10]
                        // Outer payload:
                        //   dest_hash[1] | src_hash[1] | HMAC[2] | ciphertext[16]
                        {
                            // 1. Compute ACK CRC.
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
                            // Layout: path_len_byte | type | ack_crc[4] | path_bytes[hops*bph] | zero-pad
                            // path_len_byte encodes bytes-per-hop in upper 2 bits, hop count in lower 6.
                            // Reverse incoming hops so the receiver learns the sequence of repeaters
                            // back to us. Cap so the inner buffer stays within 2 AES blocks (32B);
                            // longer paths fall back to 0 hops.
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
                            inner[1] = MESHCORE_PAYLOAD_TYPE_ACK;
                            inner[2] = sha_out[0];
                            inner[3] = sha_out[1];
                            inner[4] = sha_out[2];
                            inner[5] = sha_out[3];
                            for (uint8_t h = 0; h < ret_hop_count; h++) {
                                uint8_t src_hop = ret_hop_count - 1 - h;
                                memcpy(&inner[6 + h * ret_bph],
                                       &mc_msg.path[src_hop * ret_bph],
                                       ret_bph);
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

                            // 3. MAC = HMAC-SHA256(good_secret[32], ciphertext)[0:2].
                            uint8_t path_mac[32];
                            mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                                            good_secret, 32, path_cipher, inner_size, path_mac);

                            // 4. Build PATH packet (outer still floods).
                            meshcore_message_t path_msg = {0};
                            path_msg.type          = MESHCORE_PAYLOAD_TYPE_PATH;
                            path_msg.route         = MESHCORE_ROUTE_TYPE_FLOOD;
                            path_msg.version       = 0;
                            path_msg.path_length   = 0;
                            path_msg.payload[0]    = src_hash;
                            path_msg.payload[1]    = node_pub_key[0];
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

void radio_start_tasks(void) {
    if (rx_mutex == NULL) rx_mutex = xSemaphoreCreateMutex();
    xTaskCreate(lora_rx_task,     "lora_rx",     10240, NULL, 5, NULL);
    xTaskCreate(advert_task,      "lora_advert",  6144, NULL, 4, NULL);
    xTaskCreate(noise_floor_task, "noise_poll",   3072, NULL, 3, NULL);
}
