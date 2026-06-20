// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
// SPDX-FileContributor: Ilias el Matani <hello@ilias.codes>

#include "identity.h"
#include <stdint.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "ed25519.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"

#define NVS_IDENTITY_SEED "node.seed"
#define NVS_LAST_TIME     "last_time_s"

static const char* TAG = "identity";

uint8_t node_pub_key[32] = {0};
uint8_t node_prv_key[64] = {0};

static bool s_ready       = false;
static bool s_sntp_synced = false;

bool ed25519_tv1_keypair_ok = false;
bool ed25519_tv1_sign_ok    = false;

bool identity_is_ready(void) {
    return s_ready;
}
bool identity_sntp_synced(void) {
    return s_sntp_synced;
}

void identity_mark_time_synced(void) {
    s_sntp_synced = true;
    time_t now    = time(NULL);
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
    bool         need_save = false;
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
        static const uint8_t tv_prv[64] = {0xa5, 0x46, 0xe3, 0x6b, 0xf0, 0x52, 0x7c, 0x9d, 0x3b, 0x16, 0x15, 0x4b, 0x82,
                                           0x46, 0x5e, 0xdd, 0x62, 0x14, 0x4c, 0x0a, 0xc1, 0xfc, 0x5a, 0x18, 0x50, 0x6a,
                                           0x22, 0x44, 0xba, 0x44, 0x9a, 0xc4, 0,    0,    0,    0,    0,    0,    0,
                                           0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
                                           0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0};
        static const uint8_t tv_pub[32] = {0xe6, 0xdb, 0x68, 0x67, 0x58, 0x30, 0x30, 0xdb, 0x35, 0x94, 0xc1,
                                           0xa4, 0x24, 0xb1, 0x5f, 0x7c, 0x72, 0x66, 0x24, 0xec, 0x26, 0xb3,
                                           0x35, 0x3b, 0x10, 0xa9, 0x03, 0xa6, 0xd0, 0xab, 0x1c, 0x4c};
        static const uint8_t tv_expected[32] = {0xc3, 0xda, 0x55, 0x37, 0x9d, 0xe9, 0xc6, 0x90, 0x8e, 0x94, 0xea,
                                                0x4d, 0xf2, 0x8d, 0x08, 0x4f, 0x32, 0xec, 0xcf, 0x03, 0x49, 0x1c,
                                                0x71, 0xf7, 0x54, 0xb4, 0x07, 0x55, 0x77, 0xa2, 0x85, 0x52};
        uint8_t              tv_result[32];
        ed25519_key_exchange_raw(tv_result, tv_pub, tv_prv);
        if (memcmp(tv_result, tv_expected, 32) == 0) {
            ESP_LOGI(TAG, "X25519 RFC7748 test vector: PASS");
        } else {
            ESP_LOGE(TAG, "X25519 RFC7748 test vector: FAIL got %02X%02X%02X%02X exp %02X%02X%02X%02X", tv_result[0],
                     tv_result[1], tv_result[2], tv_result[3], tv_expected[0], tv_expected[1], tv_expected[2],
                     tv_expected[3]);
        }
    }

    // Self-consistency check A: conv(Ed25519 base point G) must equal u=9.
    {
        static const uint8_t ed25519_G[32] = {0x58, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
                                              0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
                                              0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66};
        static const uint8_t u9[32]        = {9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                              0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        uint8_t              conv_G[32];
        ed25519_pub_to_x25519(conv_G, ed25519_G);
        if (memcmp(conv_G, u9, 32) == 0) {
            ESP_LOGI(TAG, "conv(G)=9: PASS");
        } else {
            ESP_LOGE(TAG, "conv(G)=9 FAIL got=%02X%02X%02X%02X", conv_G[0], conv_G[1], conv_G[2], conv_G[3]);
        }
    }

    // Self-consistency check B: conv(node_pub_key) must equal X25519(our_scalar, 9).
    {
        static const uint8_t base9[32] = {9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                          0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        uint8_t              via_scalar[32], via_conv[32];
        ed25519_key_exchange_raw(via_scalar, base9, node_prv_key);
        ed25519_pub_to_x25519(via_conv, node_pub_key);
        if (memcmp(via_scalar, via_conv, 32) == 0) {
            ESP_LOGI(TAG, "DH self-check: PASS");
        } else {
            ESP_LOGE(TAG, "DH self-chk FAIL sc=%02X%02X cv=%02X%02X", via_scalar[0], via_scalar[1], via_conv[0],
                     via_conv[1]);
        }
    }

    ESP_LOGI(TAG,
             "Pub key: "
             "%02X%02X%02X%02X%02X%02X%02X%02X"
             "%02X%02X%02X%02X%02X%02X%02X%02X"
             "%02X%02X%02X%02X%02X%02X%02X%02X"
             "%02X%02X%02X%02X%02X%02X%02X%02X",
             node_pub_key[0], node_pub_key[1], node_pub_key[2], node_pub_key[3], node_pub_key[4], node_pub_key[5],
             node_pub_key[6], node_pub_key[7], node_pub_key[8], node_pub_key[9], node_pub_key[10], node_pub_key[11],
             node_pub_key[12], node_pub_key[13], node_pub_key[14], node_pub_key[15], node_pub_key[16], node_pub_key[17],
             node_pub_key[18], node_pub_key[19], node_pub_key[20], node_pub_key[21], node_pub_key[22], node_pub_key[23],
             node_pub_key[24], node_pub_key[25], node_pub_key[26], node_pub_key[27], node_pub_key[28], node_pub_key[29],
             node_pub_key[30], node_pub_key[31]);

    /* RFC 8032 TV1 sign-roundtrip self-test. Catches regressions in the
     * Ed25519 impl (keypair derivation + deterministic sign). Verbose only
     * on failure -- a green boot just logs one PASS line. */
    {
        static const uint8_t tv_seed[32]         = {0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60, 0xba, 0x84, 0x4a,
                                                    0xf4, 0x92, 0xec, 0x2c, 0xc4, 0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32,
                                                    0x69, 0x19, 0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60};
        static const uint8_t tv_pub_expected[32] = {0xd7, 0x5a, 0x98, 0x01, 0x82, 0xb1, 0x0a, 0xb7, 0xd5, 0x4b, 0xfe,
                                                    0xd3, 0xc9, 0x64, 0x07, 0x3a, 0x0e, 0xe1, 0x72, 0xf3, 0xda, 0xa6,
                                                    0x23, 0x25, 0xaf, 0x02, 0x1a, 0x68, 0xf7, 0x07, 0x51, 0x1a};
        static const uint8_t tv_sig_expected[64] = {
            0xe5, 0x56, 0x43, 0x00, 0xc3, 0x60, 0xac, 0x72, 0x90, 0x86, 0xe2, 0xcc, 0x80, 0x6e, 0x82, 0x8a,
            0x84, 0x87, 0x7f, 0x1e, 0xb8, 0xe5, 0xd9, 0x74, 0xd8, 0x73, 0xe0, 0x65, 0x22, 0x49, 0x01, 0x55,
            0x5f, 0xb8, 0x82, 0x15, 0x90, 0xa3, 0x3b, 0xac, 0xc6, 0x1e, 0x39, 0x70, 0x1c, 0xf9, 0xb4, 0x6b,
            0xd2, 0x5b, 0xf5, 0xf0, 0x59, 0x5b, 0xbe, 0x24, 0x65, 0x51, 0x41, 0x43, 0x8e, 0x7a, 0x10, 0x0b};
        uint8_t tv_pub[32], tv_prv[64], tv_sig[64];
        ed25519_create_keypair(tv_pub, tv_prv, tv_seed);
        ed25519_tv1_keypair_ok = (memcmp(tv_pub, tv_pub_expected, 32) == 0);
        ed25519_sign(tv_sig, NULL, 0, tv_pub, tv_prv);
        ed25519_tv1_sign_ok = (memcmp(tv_sig, tv_sig_expected, 64) == 0);
        if (ed25519_tv1_keypair_ok && ed25519_tv1_sign_ok) {
            ESP_LOGI(TAG, "RFC8032 TV1: PASS");
        } else {
            /* Hard-fail: if the build's Ed25519 produces wrong output for the
             * RFC 8032 test vectors, every outgoing advert/DM signature will
             * be rejected by upstream MeshCore verifiers. Refusing to start
             * is louder than running silently broken (the original bug spent
             * months hidden because the sender never sees rejections). The
             * device will reboot to launcher; CI's host-side test catches
             * this before merge, but this is the runtime backstop. */
            ESP_LOGE(TAG, "RFC8032 TV1 FAIL: keypair=%s sign=%s — ABORTING", ed25519_tv1_keypair_ok ? "PASS" : "FAIL",
                     ed25519_tv1_sign_ok ? "PASS" : "FAIL");
            ESP_LOGE(TAG,
                     "Crypto is broken; refusing to start. See "
                     "tests/test_ed25519.c for the host-side gate.");
            abort();
        }
    }
}
