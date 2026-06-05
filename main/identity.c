// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "identity.h"

#include <stdint.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_sntp.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "ed25519.h"

#define NVS_IDENTITY_SEED "node.seed"
#define NVS_LAST_TIME     "last_time_s"

static const char *TAG = "identity";

uint8_t node_pub_key[32] = {0};
uint8_t node_prv_key[64] = {0};

static bool s_ready       = false;
static bool s_sntp_synced = false;

bool identity_is_ready(void)     { return s_ready;       }
bool identity_sntp_synced(void)  { return s_sntp_synced; }

void identity_sntp_sync_cb(struct timeval *tv) {
    s_sntp_synced = true;
    // Persist to NVS so the next boot without WiFi can restore a sane time.
    nvs_handle_t h;
    if (nvs_open("system", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i64(h, NVS_LAST_TIME, (int64_t)tv->tv_sec);
        nvs_commit(h);
        nvs_close(h);
    }
}

void identity_mark_time_synced(void) {
    s_sntp_synced = true;
    time_t now = time(NULL);
    if (now < 1000000000LL) return;  // refuse to persist obvious garbage
    nvs_handle_t h;
    if (nvs_open("system", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i64(h, NVS_LAST_TIME, (int64_t)now);
        nvs_commit(h);
        nvs_close(h);
    }
}

void identity_init(void) {
    uint8_t seed[32] = {0};

    nvs_handle_t handle;
    bool need_save = false;
    if (nvs_open("system", NVS_READWRITE, &handle) == ESP_OK) {
        size_t len = sizeof(seed);
        if (nvs_get_blob(handle, NVS_IDENTITY_SEED, seed, &len) != ESP_OK || len != 32) {
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
        esp_fill_random(seed, sizeof(seed));
        ESP_LOGW(TAG, "NVS unavailable — ephemeral identity");
    }

    ed25519_create_keypair(node_pub_key, node_prv_key, seed);
    s_ready = true;

    // RFC 7748 X25519 test vector — verifies our Montgomery ladder.
    {
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
        }
    }

    // Self-consistency check A: conv(Ed25519 base point G) must equal u=9.
    {
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
            ESP_LOGE(TAG, "conv(G)=9 FAIL got=%02X%02X%02X%02X",
                     conv_G[0], conv_G[1], conv_G[2], conv_G[3]);
        }
    }

    // Self-consistency check B: conv(node_pub_key) must equal X25519(our_scalar, 9).
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
            ESP_LOGE(TAG, "DH self-chk FAIL sc=%02X%02X cv=%02X%02X",
                     via_scalar[0], via_scalar[1], via_conv[0], via_conv[1]);
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
