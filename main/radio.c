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
#include "channels.h"
#include "contacts.h"
#include "identity.h"
#include "nodes.h"
#include "region_limits.h"
#include "settings_nvs.h"

static const char *TAG = "radio";

// Upstream MeshCore region scope: when our region_scope NVS string is non-empty,
// we send packets as ROUTE_TYPE_TRANSPORT_FLOOD with a 4-byte transport_codes
// prefix. transport_codes[0] = HMAC-SHA256(SHA256(region_name)[0..15],
// type || payload)[0..1]. Receivers/repeaters that know the same region key
// can verify the code and route within-scope. transport_codes[1] is reserved
// for sender's "home" region (upstream marks it REVISIT — left zero for now).
//
// Reference: helpers/TransportKeyStore.cpp::calcTransportCode + MyMesh.cpp
// (examples/companion_radio) sendFloodScoped path.
static void apply_region_scope(meshcore_message_t *msg) {
    if (!region_scope[0]) return;  // no scope: stay on plain FLOOD

    // Upstream MeshCore RegionMap::getTransportKeysFor prepends '#' to the
    // region name before SHA256-deriving the transport key. Match that or our
    // HMAC code differs from what scope-aware relays compute and they drop us.
    char scope_name[35];
    if (region_scope[0] == '#') {
        snprintf(scope_name, sizeof(scope_name), "%s", region_scope);
    } else {
        snprintf(scope_name, sizeof(scope_name), "#%s", region_scope);
    }

    uint8_t region_key[16];
    {
        uint8_t digest[32];
        mbedtls_sha256((const uint8_t *)scope_name, strlen(scope_name), digest, 0);
        memcpy(region_key, digest, sizeof(region_key));
    }

    uint8_t type_byte = (uint8_t)msg->type;
    uint8_t mac[32];
    {
        mbedtls_md_context_t md;
        mbedtls_md_init(&md);
        mbedtls_md_setup(&md, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
        mbedtls_md_hmac_starts(&md, region_key, sizeof(region_key));
        mbedtls_md_hmac_update(&md, &type_byte, 1);
        mbedtls_md_hmac_update(&md, msg->payload, msg->payload_length);
        mbedtls_md_hmac_finish(&md, mac);
        mbedtls_md_free(&md);
    }
    uint16_t code;
    memcpy(&code, mac, 2);
    if (code == 0x0000)      code = 0x0001;
    else if (code == 0xFFFF) code = 0xFFFE;

    msg->route             = MESHCORE_ROUTE_TYPE_TRANSPORT_FLOOD;
    msg->transport_codes[0] = code;
    msg->transport_codes[1] = 0;  // home region — upstream REVISIT, leave zero
}

// Shared LoRa handle for radio v3.0.0 API. Single SDIO-connected (remote) radio.
lora_handle_t lora_handle = {0};

// Filled in main.c after lora_init_remote() + lora_get_status() succeeds.
char radio_fw_version[RADIO_FW_VERSION_LEN] = "?";

// Filled in main.c via lora_get_fw_version() after lora_get_status(); stays
// empty if C6 firmware lacks GET_FW_VERSION (0x0B) — render.c then falls
// back to the hand-maintained TANMATSU_RADIO_FW_LABEL define.
char radio_fw_app_version[RADIO_FW_APP_VERSION_LEN] = "";

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

// ── Duty-cycle accounting (rolling 1-hour, 1-minute bucket granularity) ──────
// 60 buckets of airtime-ms, indexed by wall-minute. Rotate on minute change.
// `dc_total_ms` is the cached sum across all buckets so render can read it
// without rescanning. Updated under `rx_mutex` (re-used to keep TX paths +
// readers cheap). All TX paths feed `dc_record_tx(airtime_ms)`. The pre-TX
// check `dc_budget_available()` blocks send when the configured country's
// sub-band budget is exhausted; the budget is informational if country=="--".
static uint32_t dc_buckets[60]      = {0};
static uint8_t  dc_head_min         = 0;
static uint32_t dc_total_ms         = 0;
static uint32_t dc_last_rotate_ms   = 0;
volatile uint32_t dc_used_ms        = 0;   // mirror of dc_total_ms for render
volatile uint32_t dc_budget_ms      = 0;   // from current sub-band, 0 = unlimited
volatile bool     dc_last_tx_blocked = false;  // sticky flag, render clears via UI

static const regulatory_subband_t *active_subband(void) {
    const regulatory_country_t *rc = region_get_country(country_code);
    if (!rc || rc->n_subbands == 0) return NULL;
    return region_match_subband(rc, (float)lora_cfg.frequency / 1000000.0f);
}

// Semtech AN1200.13 / SX1276 datasheet airtime formula (works for SX126x too).
// Returns time-on-air in milliseconds for the current lora_cfg given a payload
// of `payload_bytes` (the full LoRa packet payload incl. header byte).
static uint32_t compute_airtime_ms(uint16_t payload_bytes) {
    uint8_t  sf = lora_cfg.spreading_factor;
    uint16_t bw_khz = lora_cfg.bandwidth;  // already in kHz per settings convention
    uint8_t  cr = lora_cfg.coding_rate;    // 5..8 → CR_value = cr-4
    uint16_t preamble = lora_cfg.preamble_length;
    uint8_t  ldr = lora_cfg.low_data_rate_optimization ? 1 : 0;
    if (bw_khz == 0 || sf < 6 || sf > 12) return 0;

    // T_sym (µs) = (1 << SF) * 1000 / BW_kHz  (precise: 2^SF / BW)
    uint32_t t_sym_us = ((uint32_t)1 << sf) * 1000u / bw_khz;

    // Preamble symbols = preamble + 4.25 → use *100 fixed point
    uint32_t n_pre_x100 = (uint32_t)preamble * 100u + 425u;

    // Payload symbol count per Semtech AN1200.13:
    // n_payload = 8 + max(ceil((8*PL - 4*SF + 28 + 16*CRC - 20*IH) / (4*(SF - 2*DE))) * (CR_val+4), 0)
    // CRC=1, IH=0 (explicit header)
    int32_t num = 8 * (int32_t)payload_bytes - 4 * (int32_t)sf + 28 + 16;
    int32_t den = 4 * ((int32_t)sf - 2 * (int32_t)ldr);
    if (den <= 0) return 0;
    int32_t ceil_div = (num + den - 1) / den;
    if (ceil_div < 0) ceil_div = 0;
    int32_t n_payload = 8 + ceil_div * (int32_t)(cr - 4 + 4);

    // Total µs = (n_pre + n_payload) * t_sym
    uint64_t total_us = ((uint64_t)n_pre_x100 + (uint64_t)n_payload * 100u) * t_sym_us / 100u;
    return (uint32_t)((total_us + 999u) / 1000u);
}

static void dc_rotate_if_needed(uint32_t now_ms) {
    uint32_t elapsed = now_ms - dc_last_rotate_ms;
    if (elapsed < 60000u) return;
    uint32_t advance = elapsed / 60000u;
    if (advance > 60u) advance = 60u;
    for (uint32_t i = 0; i < advance; i++) {
        dc_head_min = (uint8_t)((dc_head_min + 1u) % 60u);
        dc_total_ms -= dc_buckets[dc_head_min];
        dc_buckets[dc_head_min] = 0;
    }
    dc_last_rotate_ms += advance * 60000u;
    dc_used_ms = dc_total_ms;
}

static bool dc_budget_available(uint32_t need_ms) {
    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    dc_rotate_if_needed(now_ms);
    const regulatory_subband_t *sb = active_subband();
    uint32_t budget = sb ? region_dc_budget_ms_per_hour(sb) : 0;
    dc_budget_ms = budget;
    // budget == 0 means either no sub-band match (country unset → don't enforce
    // anything) or 100% DC (no limit). Either way, allow.
    if (budget == 0 || budget >= 3600000u) return true;
    return (dc_total_ms + need_ms) <= budget;
}

static void dc_record_tx(uint32_t airtime_ms) {
    if (airtime_ms == 0) return;
    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    dc_rotate_if_needed(now_ms);
    dc_buckets[dc_head_min] += airtime_ms;
    dc_total_ms             += airtime_ms;
    dc_used_ms               = dc_total_ms;
    dc_last_tx_blocked       = false;  // recovered — a TX got through
}

// ── ADVERT TX ────────────────────────────────────────────────────────────────
void send_advert(void) {
    if (!c6_available || !identity_is_ready()) return;

    meshcore_advert_t advert = {0};
    memcpy(advert.pub_key, node_pub_key, MESHCORE_PUB_KEY_SIZE);
    // UNIX epoch — matches upstream MeshCore convention and what the other
    // payload paths in this file use. Earlier versions accidentally used
    // xTaskGetTickCount-derived uptime here, which other clients rejected
    // because the timestamp looked decades old (~15 since boot).
    advert.timestamp = (uint32_t)time(NULL);
    advert.role      = lora_role;

    const char *adv_src = lora_advert_name[0] ? lora_advert_name :
                          ((owner_name[0] && owner_name[0] != '(') ? owner_name : NULL);
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
    msg.bytes_per_hop  = path_hash_size;
    msg.payload_length = payload_len;
    memcpy(msg.payload, payload, payload_len);
    apply_region_scope(&msg);

    uint8_t pkt_data[MESHCORE_MAX_TRANS_UNIT];
    uint8_t pkt_len = 0;
    if (meshcore_serialize(&msg, pkt_data, &pkt_len) < 0) return;

    lora_protocol_lora_packet_t pkt = {0};
    pkt.length = pkt_len;
    memcpy(pkt.data, pkt_data, pkt_len);

    uint32_t airtime_ms = compute_airtime_ms(pkt_len);
    if (!dc_budget_available(airtime_ms)) {
        dc_last_tx_blocked = true;
        ESP_LOGW(TAG, "ADVERT skipped: duty-cycle budget exhausted "
                       "(%lums used / %lums budget, +%lums)",
                 (unsigned long)dc_used_ms, (unsigned long)dc_budget_ms,
                 (unsigned long)airtime_ms);
        return;
    }

    esp_err_t res = lora_send_packet(&lora_handle, &pkt);

    if (res == ESP_OK) {
        dc_record_tx(airtime_ms);
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
bool send_dm_message(const char *text, const uint8_t *target_pub, uint8_t ack_crc_out[4]) {
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

    // ACK CRC: the receiver computes SHA256(plain[0..5] || text || OUR pubkey)[0:4]
    // to bind the ACK to this specific message. Mirror that here so the caller
    // can store it on the chat_msg and detect the matching PATH_RETURN later.
    if (ack_crc_out) {
        uint8_t sha_out[32];
        mbedtls_sha256_context sha_ctx;
        mbedtls_sha256_init(&sha_ctx);
        mbedtls_sha256_starts(&sha_ctx, 0);
        mbedtls_sha256_update(&sha_ctx, plain, 5);
        mbedtls_sha256_update(&sha_ctx, (const uint8_t*)text, text_len);
        mbedtls_sha256_update(&sha_ctx, node_pub_key, MESHCORE_PUB_KEY_SIZE);
        mbedtls_sha256_finish(&sha_ctx, sha_out);
        mbedtls_sha256_free(&sha_ctx);
        memcpy(ack_crc_out, sha_out, 4);
    }

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
    msg.bytes_per_hop  = path_hash_size;
    msg.path_length    = 0;
    msg.payload[0]     = target_pub[0];     // dest hash
    msg.payload[1]     = node_pub_key[0];   // src  hash
    msg.payload[2]     = mac[0];
    msg.payload[3]     = mac[1];
    memcpy(&msg.payload[4], cipher, padded);
    msg.payload_length = (uint8_t)(4 + padded);
    apply_region_scope(&msg);

    uint8_t pkt_data[MESHCORE_MAX_TRANS_UNIT];
    uint8_t pkt_len = 0;
    if (meshcore_serialize(&msg, pkt_data, &pkt_len) < 0) return false;

    lora_protocol_lora_packet_t pkt = {0};
    pkt.length = pkt_len;
    memcpy(pkt.data, pkt_data, pkt_len);

    uint32_t airtime_ms = compute_airtime_ms(pkt_len);
    if (!dc_budget_available(airtime_ms)) {
        dc_last_tx_blocked = true;
        ESP_LOGW(TAG, "DM skipped: duty-cycle budget exhausted");
        return false;
    }

    bool ok = (lora_send_packet(&lora_handle, &pkt) == ESP_OK);
    if (ok) dc_record_tx(airtime_ms);
    return ok;
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

    // Pick the active channel's key + hash; fall back to Public (idx 0) if
    // active_channel_idx is somehow out of range.
    int ch_idx = (active_channel_idx >= 0 && active_channel_idx < channel_count &&
                  channels[active_channel_idx].active)
                 ? active_channel_idx : 0;
    const uint8_t *ch_secret = channels[ch_idx].secret;
    uint8_t        ch_hash   = channels[ch_idx].hash;

    uint8_t cipher[MESHCORE_MAX_PAYLOAD_SIZE] = {0};
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, ch_secret, 128);
    for (size_t i = 0; i < padded / MESHCORE_CIPHER_BLOCK_SIZE; i++) {
        mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT,
                               &plain[i * MESHCORE_CIPHER_BLOCK_SIZE],
                               &cipher[i * MESHCORE_CIPHER_BLOCK_SIZE]);
    }
    mbedtls_aes_free(&aes);

    uint8_t mac[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                    ch_secret, MESHCORE_CIPHER_KEY_SIZE,
                    cipher, (uint16_t)padded, mac);

    meshcore_grp_txt_t grp = {0};
    grp.channel_hash = ch_hash;
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
    msg.bytes_per_hop  = path_hash_size;
    msg.path_length    = 0;
    msg.payload_length = payload_len;
    memcpy(msg.payload, payload, payload_len);
    apply_region_scope(&msg);

    uint8_t pkt_data[MESHCORE_MAX_TRANS_UNIT];
    uint8_t pkt_len = 0;
    if (meshcore_serialize(&msg, pkt_data, &pkt_len) < 0) return false;

    lora_protocol_lora_packet_t pkt = {0};
    pkt.length = pkt_len;
    memcpy(pkt.data, pkt_data, pkt_len);

    uint32_t airtime_ms = compute_airtime_ms(pkt_len);
    if (!dc_budget_available(airtime_ms)) {
        dc_last_tx_blocked = true;
        ESP_LOGW(TAG, "Chat skipped: duty-cycle budget exhausted");
        return false;
    }

    esp_err_t res = lora_send_packet(&lora_handle, &pkt);

    if (res == ESP_OK) {
        dc_record_tx(airtime_ms);
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
        float     rssi = 0.0f;
        esp_err_t r    = lora_get_rssi_inst(&lora_handle, &rssi);  // v0.2.1: dBm as float
        if (r == ESP_OK) {
            int dbm = (int)rssi;
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
        esp_err_t res = lora_receive_packet(&lora_handle, &pkt, pdMS_TO_TICKS(10000));
        if (res == ESP_OK && pkt.length > 0) {
            uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

            int rssi_dbm        = -(int)pkt.stats.rssi_pkt_raw / 2;
            int signal_rssi_dbm = -(int)pkt.stats.signal_rssi_pkt_raw / 2;
            if (rssi_dbm < -127)        rssi_dbm = -127;
            if (signal_rssi_dbm < -127) signal_rssi_dbm = -127;
            last_rx_rssi_dbm        = (int8_t)rssi_dbm;
            last_rx_snr_db_x4       = pkt.stats.snr_pkt_raw;
            last_rx_signal_rssi_dbm = (int8_t)signal_rssi_dbm;
            last_rx_stats_ms        = now_ms;
            last_rx_stats_valid     = true;

            ESP_LOGI(TAG, "RX %d bytes: %02X %02X %02X %02X (rssi=%d snr=%d/4)",
                     pkt.length,
                     pkt.length > 0 ? pkt.data[0] : 0,
                     pkt.length > 1 ? pkt.data[1] : 0,
                     pkt.length > 2 ? pkt.data[2] : 0,
                     pkt.length > 3 ? pkt.data[3] : 0,
                     -(int)pkt.stats.rssi_pkt_raw / 2,
                     (int)pkt.stats.snr_pkt_raw);

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
                        update_node(&advert, now_ms, &pkt.stats);
                    }
                } else if (mc_msg.type == MESHCORE_PAYLOAD_TYPE_GRP_TXT) {
                    meshcore_grp_txt_t grp = {0};
                    if (meshcore_grp_txt_deserialize(mc_msg.payload, mc_msg.payload_length, &grp) >= 0) {
                        // Brute-force over all configured channels. The wire
                        // channel_hash is only an optimisation hint — we use
                        // the matching channel index to pick the secret, but
                        // accept on MAC verify only. If hash doesn't match
                        // anything we still try every key (some senders set
                        // hash=0x00 even though they encrypt with a real key).
                        int hit = channels_find_by_hash(grp.channel_hash);
                        int start = (hit >= 0) ? hit : 0;
                        int end   = (hit >= 0) ? hit + 1 : channel_count;
                        bool decrypted = false;
                        for (int ci = start; ci < end; ci++) {
                            if (!channels[ci].active) continue;
                            meshcore_grp_txt_t attempt = grp;  // structcopy: keep original mac+data for next try
                            if (decrypt_grp_txt(&attempt, channels[ci].secret)) {
                                ESP_LOGI(TAG, "Channel RX [%s]: %s",
                                         channels[ci].name, attempt.decrypted.text);
                                // Channel context lives in the chat header; no
                                // inline [#name] prefix on the message body.
                                // Routes to channel ci's own history; only hits
                                // the visible ring if ci is the active channel.
                                bool added = ch_add_message_for(ci, attempt.decrypted.text, false);
                                if (added) {
                                    uint8_t bph  = mc_msg.bytes_per_hop ? mc_msg.bytes_per_hop : 1;
                                    uint8_t hops = (uint8_t)(mc_msg.path_length / bph);
                                    chat_set_meta_channel(hops);
                                }
                                if (!(added && current_view == VIEW_CHANNEL)) {
                                    led_channel_pending = true;
                                    channel_unread[ci]++;
                                    update_notification_led();
                                }
                                decrypted = true;
                                break;
                            }
                        }
                        if (!decrypted && hit >= 0) {
                            // Hash claimed a known channel but MAC didn't verify —
                            // fall back to trying everything else.
                            for (int ci = 0; ci < channel_count; ci++) {
                                if (ci == hit || !channels[ci].active) continue;
                                meshcore_grp_txt_t attempt = grp;
                                if (decrypt_grp_txt(&attempt, channels[ci].secret)) {
                                    ESP_LOGI(TAG, "Channel RX [%s] (hash mismatch): %s",
                                             channels[ci].name, attempt.decrypted.text);
                                    bool added = ch_add_message_for(ci, attempt.decrypted.text, false);
                                    if (added) {
                                        uint8_t bph  = mc_msg.bytes_per_hop ? mc_msg.bytes_per_hop : 1;
                                        uint8_t hops = (uint8_t)(mc_msg.path_length / bph);
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
                        {
                            uint8_t bph  = mc_msg.bytes_per_hop ? mc_msg.bytes_per_hop : 1;
                            uint8_t hops = (uint8_t)(mc_msg.path_length / bph);
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
                            path_msg.bytes_per_hop = path_hash_size;
                            path_msg.path_length   = 0;
                            path_msg.payload[0]    = src_hash;
                            path_msg.payload[1]    = node_pub_key[0];
                            path_msg.payload[2]    = path_mac[0];
                            path_msg.payload[3]    = path_mac[1];
                            memcpy(&path_msg.payload[4], path_cipher, inner_size);
                            path_msg.payload_length = 4 + inner_size;
                            apply_region_scope(&path_msg);

                            ESP_LOGI(TAG, "PATH_RETURN: hops=%u bph=%u inner_size=%u",
                                     (unsigned)ret_hop_count, (unsigned)ret_bph,
                                     (unsigned)inner_size);

                            uint8_t path_data[MESHCORE_MAX_TRANS_UNIT];
                            uint8_t path_sz = 0;
                            if (meshcore_serialize(&path_msg, path_data, &path_sz) == 0) {
                                lora_protocol_lora_packet_t lora_pkt = {0};
                                lora_pkt.length = path_sz;
                                memcpy(lora_pkt.data, path_data, path_sz);
                                uint32_t air_ms = compute_airtime_ms(path_sz);
                                if (dc_budget_available(air_ms)) {
                                    if (lora_send_packet(&lora_handle, &lora_pkt) == ESP_OK) {
                                        dc_record_tx(air_ms);
                                    }
                                } else {
                                    dc_last_tx_blocked = true;
                                    ESP_LOGW(TAG, "PATH_RETURN skipped: duty-cycle exhausted");
                                }
                            }
                        }
                    } while (0);
                } else if (mc_msg.type == MESHCORE_PAYLOAD_TYPE_PATH) {
                    // Incoming PATH_RETURN. We only care about it when it
                    // carries an ACK for one of our outgoing DMs — match by
                    // ack_crc[4] inside the encrypted inner block. The same
                    // do{}while(0) pattern lets us bail out cleanly.
                    do {
                        if (mc_msg.payload_length < 4 + 16) break;
                        uint8_t dest_hash = mc_msg.payload[0];
                        uint8_t src_hash  = mc_msg.payload[1];
                        if (dest_hash != node_pub_key[0]) break;  // not for us

                        // Find sender by src_hash among live nodes + contacts.
                        uint8_t sender_pub[32] = {0};
                        bool    sender_found  = false;
                        if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                            for (int ni = 0; ni < MAX_NODES; ni++) {
                                if (node_list[ni].active && node_list[ni].pub_key[0] == src_hash) {
                                    memcpy(sender_pub, node_list[ni].pub_key, 32);
                                    sender_found = true;
                                    break;
                                }
                            }
                            xSemaphoreGive(node_mutex);
                        }
                        if (!sender_found) {
                            for (int ci = 0; ci < contact_count; ci++) {
                                if (contacts[ci].pub_key[0] == src_hash) {
                                    memcpy(sender_pub, contacts[ci].pub_key, 32);
                                    sender_found = true;
                                    break;
                                }
                            }
                        }
                        if (!sender_found) break;

                        // Derive shared secret (try both ed25519 variants, same
                        // as the DM path does). Decrypt one or two AES blocks
                        // and see if inner[1] = ACK and inner[2..5] matches a
                        // recent own DM's CRC.
                        uint8_t secret[32];
                        ed25519_key_exchange(secret, sender_pub, node_prv_key);

                        size_t ct_len = mc_msg.payload_length - 4;
                        if (ct_len > 32) ct_len = 32;
                        ct_len = (ct_len / 16) * 16;
                        if (ct_len < 16) break;

                        uint8_t inner[32]      = {0};
                        uint8_t ciphertext[32] = {0};
                        memcpy(ciphertext, &mc_msg.payload[4], ct_len);
                        mbedtls_aes_context aes_ctx;
                        mbedtls_aes_init(&aes_ctx);
                        mbedtls_aes_setkey_dec(&aes_ctx, secret, 128);
                        for (size_t bi = 0; bi + 16 <= ct_len; bi += 16) {
                            mbedtls_aes_crypt_ecb(&aes_ctx, MBEDTLS_AES_DECRYPT,
                                                  ciphertext + bi, inner + bi);
                        }
                        mbedtls_aes_free(&aes_ctx);

                        // inner[0] = path_len_byte, inner[1] = payload type.
                        if (inner[1] != MESHCORE_PAYLOAD_TYPE_ACK) break;
                        uint8_t ack_crc[4] = { inner[2], inner[3], inner[4], inner[5] };
                        if (chat_mark_ack_by_crc(ack_crc)) {
                            ESP_LOGI(TAG, "ACK matched: %02X%02X%02X%02X from %02X",
                                     ack_crc[0], ack_crc[1], ack_crc[2], ack_crc[3], src_hash);
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
