// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "atomic_file.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static const char* TAG = "atomic_file";

bool atomic_write_file(const char* path, const void* data, size_t len) {
    if (!path) return false;

    char tmp[128];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) {
        ESP_LOGW(TAG, "path too long: %s", path);
        return false;
    }

    FILE* f = fopen(tmp, "wb");
    if (!f) {
        ESP_LOGW(TAG, "fopen(%s) failed", tmp);
        return false;
    }
    bool ok = (len == 0) || fwrite(data, 1, len, f) == len;
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        ESP_LOGW(TAG, "short write to %s, keeping the previous %s", tmp, path);
        remove(tmp);
        return false;
    }

    remove(path);  // FATFS rename fails when the target exists
    if (rename(tmp, path) != 0) {
        ESP_LOGW(TAG, "rename(%s -> %s) failed", tmp, path);
        remove(tmp);
        return false;
    }
    return true;
}
