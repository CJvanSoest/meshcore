// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "nvs_helpers.h"

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "nvs_helpers";

#define LOAD_SCALAR(type, getter)                                              \
    nvs_handle_t h;                                                            \
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return false;                \
    type v;                                                                    \
    esp_err_t r = getter(h, key, &v);                                          \
    nvs_close(h);                                                              \
    if (r != ESP_OK) return false;                                             \
    *out = v;                                                                  \
    return true

bool nvs_load_u8 (const char *ns, const char *key, uint8_t  *out) { LOAD_SCALAR(uint8_t,  nvs_get_u8 ); }
bool nvs_load_i8 (const char *ns, const char *key, int8_t   *out) { LOAD_SCALAR(int8_t,   nvs_get_i8 ); }

#undef LOAD_SCALAR

#define SAVE_SCALAR(setter)                                                    \
    nvs_handle_t h;                                                            \
    esp_err_t r = nvs_open(ns, NVS_READWRITE, &h);                             \
    if (r != ESP_OK) { ESP_LOGW(TAG, "open %s/%s: %d", ns, key, r); return false; } \
    r = setter(h, key, val);                                                   \
    if (r == ESP_OK) r = nvs_commit(h);                                        \
    nvs_close(h);                                                              \
    if (r != ESP_OK) ESP_LOGW(TAG, "save %s/%s: %d", ns, key, r);              \
    return r == ESP_OK

bool nvs_save_u8 (const char *ns, const char *key, uint8_t  val) { SAVE_SCALAR(nvs_set_u8 ); }
bool nvs_save_i8 (const char *ns, const char *key, int8_t   val) { SAVE_SCALAR(nvs_set_i8 ); }

#undef SAVE_SCALAR
