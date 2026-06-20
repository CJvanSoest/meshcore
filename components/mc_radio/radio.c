// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
// SPDX-FileContributor: Ilias el Matani <hello@ilias.codes>

#include "radio.h"
#include <string.h>
#include "esp_log.h"
#include "freertos/task.h"
#include "mc_crypto.h"
#include "meshcore/packet.h"
#include "region_limits.h"
#include "settings_nvs.h"

static const char* TAG = "radio";

// Upstream MeshCore region scope: when our region_scope NVS string is non-empty,
// we send packets as ROUTE_TYPE_TRANSPORT_FLOOD with a 4-byte transport_codes
// prefix. transport_codes[0] = HMAC-SHA256(SHA256(region_name)[0..15],
// type || payload)[0..1]. Receivers/repeaters that know the same region key
// can verify the code and route within-scope. transport_codes[1] is reserved
// for sender's "home" region (upstream marks it REVISIT — left zero for now).
//
// Reference: helpers/TransportKeyStore.cpp::calcTransportCode + MyMesh.cpp
// (examples/companion_radio) sendFloodScoped path.
static void apply_region_scope(meshcore_message_t* msg) {
    if (!region_scope[0]) return;  // no scope: stay on plain FLOOD
    msg->route = MESHCORE_ROUTE_TYPE_TRANSPORT_FLOOD;
    msg->transport_codes[0] =
        mc_crypto_region_transport_code(region_scope, (uint8_t)msg->type, msg->payload, msg->payload_length);
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

volatile int8_t noise_floor_dbm       = 0;
volatile bool   noise_floor_valid     = false;
volatile bool   noise_floor_supported = true;

uint32_t last_advert_ms = 0;

// ── Duty-cycle accounting (rolling 1-hour, 1-minute bucket granularity) ──────
// 60 buckets of airtime-ms, indexed by wall-minute. Rotate on minute change.
// `dc_total_ms` is the cached sum across all buckets so render can read it
// without rescanning. The buckets + sum are mutated under `dc_mutex` because
// several FreeRTOS tasks issue TX concurrently (the advert task, the direct-
// advert task, a PATH_RETURN ACK from the RX task, and UI sends); without it
// the read-modify-write of the rolling sum races and miscounts air time, which
// can let TX slip past the regulatory duty-cycle budget. All TX paths feed
// `dc_record_tx(airtime_ms)`. The pre-TX check `dc_budget_available()` blocks
// send when the configured country's sub-band budget is exhausted; the budget
// is informational if country=="--".
static uint32_t          dc_buckets[60]     = {0};
static uint8_t           dc_head_min        = 0;
static uint32_t          dc_total_ms        = 0;
static uint32_t          dc_last_rotate_ms  = 0;
static SemaphoreHandle_t dc_mutex           = NULL;
volatile uint32_t        dc_used_ms         = 0;      // mirror of dc_total_ms for render
volatile uint32_t        dc_budget_ms       = 0;      // from current sub-band, 0 = unlimited
volatile bool            dc_last_tx_blocked = false;  // sticky flag, render clears via UI

static const regulatory_subband_t* active_subband(void) {
    const regulatory_country_t* rc = region_get_country(country_code);
    if (!rc || rc->n_subbands == 0) return NULL;
    return region_match_subband(rc, (float)lora_cfg.frequency / 1000000.0f);
}

// Semtech AN1200.13 / SX1276 datasheet airtime formula (works for SX126x too).
// Returns time-on-air in milliseconds for the current lora_cfg given a payload
// of `payload_bytes` (the full LoRa packet payload incl. header byte).
static uint32_t compute_airtime_ms(uint16_t payload_bytes) {
    uint8_t  sf       = lora_cfg.spreading_factor;
    uint16_t bw_khz   = lora_cfg.bandwidth;    // already in kHz per settings convention
    uint8_t  cr       = lora_cfg.coding_rate;  // 5..8 → CR_value = cr-4
    uint16_t preamble = lora_cfg.preamble_length;
    uint8_t  ldr      = lora_cfg.low_data_rate_optimization ? 1 : 0;
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
        dc_head_min              = (uint8_t)((dc_head_min + 1u) % 60u);
        dc_total_ms             -= dc_buckets[dc_head_min];
        dc_buckets[dc_head_min]  = 0;
    }
    dc_last_rotate_ms += advance * 60000u;
    dc_used_ms         = dc_total_ms;
}

static bool dc_budget_available(uint32_t need_ms) {
    const regulatory_subband_t* sb     = active_subband();
    uint32_t                    budget = sb ? region_dc_budget_ms_per_hour(sb) : 0;
    dc_budget_ms                       = budget;
    // budget == 0 means either no sub-band match (country unset → don't enforce
    // anything) or 100% DC (no limit). Either way, allow.
    if (budget == 0 || budget >= 3600000u) return true;
    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    uint32_t total;
    if (dc_mutex) xSemaphoreTake(dc_mutex, portMAX_DELAY);
    dc_rotate_if_needed(now_ms);
    total = dc_total_ms;
    if (dc_mutex) xSemaphoreGive(dc_mutex);
    return (total + need_ms) <= budget;
}

static void dc_record_tx(uint32_t airtime_ms) {
    if (airtime_ms == 0) return;
    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (dc_mutex) xSemaphoreTake(dc_mutex, portMAX_DELAY);
    dc_rotate_if_needed(now_ms);
    dc_buckets[dc_head_min] += airtime_ms;
    dc_total_ms             += airtime_ms;
    dc_used_ms               = dc_total_ms;
    dc_last_tx_blocked       = false;  // recovered — a TX got through
    if (dc_mutex) xSemaphoreGive(dc_mutex);
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

static bool rx_is_duplicate(const uint8_t* payload, uint16_t payload_len) {
    uint8_t fp[16] = {0};
    int     fp_len = payload_len < 16 ? payload_len : 16;
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
static void noise_floor_task(void* arg) {
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

// ── RX sink + shared TX primitive ───────────────────────────────────────────
// The RX path is domain-free: radio deserializes + dedups and hands the raw
// typed message to the registered sink (mc_rx), which owns decrypt, domain
// writes and notifications. radio_tx_message is the shared TX tail (region
// scope + serialize + duty-cycle gate + send) used by the TX paths and by the
// sink's PATH_RETURN ACK.

static radio_rx_sink_fn s_rx_sink = NULL;

void radio_set_rx_sink(radio_rx_sink_fn sink) {
    s_rx_sink = sink;
}

bool radio_tx_message(meshcore_message_t* msg) {
    apply_region_scope(msg);

    uint8_t pkt_data[MESHCORE_MAX_TRANS_UNIT];
    uint8_t pkt_len = 0;
    if (meshcore_serialize(msg, pkt_data, &pkt_len) < 0) return false;

    lora_protocol_lora_packet_t pkt = {0};
    pkt.length                      = pkt_len;
    memcpy(pkt.data, pkt_data, pkt_len);

    uint32_t airtime_ms = compute_airtime_ms(pkt_len);
    if (!dc_budget_available(airtime_ms)) {
        dc_last_tx_blocked = true;
        ESP_LOGW(TAG, "TX skipped: duty-cycle budget exhausted");
        return false;
    }
    bool ok = (lora_send_packet(&lora_handle, &pkt) == ESP_OK);
    if (ok) dc_record_tx(airtime_ms);
    return ok;
}

static void lora_rx_task(void* arg) {
    (void)arg;
    ESP_LOGI(TAG, "LoRa RX task started");
    while (1) {
        lora_protocol_lora_packet_t pkt = {0};
        esp_err_t                   res = lora_receive_packet(&lora_handle, &pkt, pdMS_TO_TICKS(10000));
        if (res != ESP_OK || pkt.length <= 0) continue;

        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

        int rssi_dbm        = -(int)pkt.stats.rssi_pkt_raw / 2;
        int signal_rssi_dbm = -(int)pkt.stats.signal_rssi_pkt_raw / 2;
        if (rssi_dbm < -127) rssi_dbm = -127;
        if (signal_rssi_dbm < -127) signal_rssi_dbm = -127;
        last_rx_rssi_dbm        = (int8_t)rssi_dbm;
        last_rx_snr_db_x4       = pkt.stats.snr_pkt_raw;
        last_rx_signal_rssi_dbm = (int8_t)signal_rssi_dbm;
        last_rx_stats_ms        = now_ms;
        last_rx_stats_valid     = true;

        ESP_LOGI(TAG, "RX %d bytes: %02X %02X %02X %02X (rssi=%d snr=%d/4)", pkt.length,
                 pkt.length > 0 ? pkt.data[0] : 0, pkt.length > 1 ? pkt.data[1] : 0, pkt.length > 2 ? pkt.data[2] : 0,
                 pkt.length > 3 ? pkt.data[3] : 0, -(int)pkt.stats.rssi_pkt_raw / 2, (int)pkt.stats.snr_pkt_raw);

        if (xSemaphoreTake(rx_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            rx_buf[rx_head].pkt          = pkt;
            rx_buf[rx_head].timestamp_ms = now_ms;
            rx_head                      = (rx_head + 1) % RX_BUF_SIZE;
            if (rx_count < RX_BUF_SIZE) rx_count++;
            xSemaphoreGive(rx_mutex);
        }

        meshcore_message_t mc_msg;
        if (meshcore_deserialize(pkt.data, pkt.length, &mc_msg) < 0) continue;
        if (rx_is_duplicate(mc_msg.payload, mc_msg.payload_length)) {
            ESP_LOGI(TAG, "Dedup: drop flood retransmit (type=%d)", mc_msg.type);
            continue;
        }

        if (s_rx_sink) {
            radio_rx_meta_t meta = {
                .rssi_dbm        = (int8_t)rssi_dbm,
                .snr_db_x4       = pkt.stats.snr_pkt_raw,
                .signal_rssi_dbm = (int8_t)signal_rssi_dbm,
                .now_ms          = now_ms,
                .stats           = pkt.stats,
            };
            s_rx_sink(&mc_msg, &meta);
        }
    }
}

void radio_start_tasks(void) {
    if (rx_mutex == NULL) rx_mutex = xSemaphoreCreateMutex();
    if (dc_mutex == NULL) dc_mutex = xSemaphoreCreateMutex();
    xTaskCreate(lora_rx_task, "lora_rx", 10240, NULL, 5, NULL);
    xTaskCreate(noise_floor_task, "noise_poll", 3072, NULL, 3, NULL);
}

// ── LoRa config reconcile (moved from settings_nvs.c) ────────────────────────
// These own lora_handle access, so they live with the radio rather than in the
// L1 config store. Bodies are unchanged from the originals.
void load_lora_config(void) {
    load_lora_from_nvs();
    c6_available = false;

    lora_protocol_config_params_t c6_cfg = {0};
    esp_err_t                     res    = lora_get_config(&lora_handle, &c6_cfg);
    if (res == ESP_OK) {
        c6_available = true;
        if (c6_cfg.frequency != 0) {
            lora_cfg = c6_cfg;
            save_lora_to_nvs();
            ESP_LOGI(TAG, "LoRa config from C6: freq=%luHz sf=%d", (unsigned long)lora_cfg.frequency,
                     lora_cfg.spreading_factor);
        } else {
            ESP_LOGI(TAG, "C6 has empty config, pushing NVS values to C6");
            lora_set_config(&lora_handle, &lora_cfg);
        }
    } else {
        ESP_LOGW(TAG, "C6 unavailable (err=%d) — using NVS values", res);
    }
}

void save_lora_config(void) {
    save_lora_to_nvs();
    if (!c6_available) return;
    esp_err_t res = lora_set_config(&lora_handle, &lora_cfg);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "lora_set_config failed: %d", res);
    } else {
        ESP_LOGI(TAG, "LoRa config pushed to C6");
        // lora_set_config resets the radio to standby — re-enter RX so we keep
        // listening on the new frequency/SF.
        if (lora_rx_ok) {
            lora_set_mode(&lora_handle, LORA_PROTOCOL_MODE_RX);
        }
    }
}
