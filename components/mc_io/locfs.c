// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "locfs.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"

#define LOCFS_PARTITION "locfd"

static const char* TAG         = "locfs";
static bool        s_ready     = false;
static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;

void locfs_init(void) {
    if (s_ready) return;

    // Deliberately NOT format_if_mount_failed: never reformat a partition we may
    // not own. If it isn't a mountable FAT (blank / firmware-held / absent) we
    // stay on NVS.
    const esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = false,
        .max_files              = 4,
        .allocation_unit_size   = 4096,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(LOCFS_MOUNT, LOCFS_PARTITION, &cfg, &s_wl_handle);
    if (err == ESP_OK) {
        s_ready = true;
        ESP_LOGI(TAG, "internal FAT store mounted at %s", LOCFS_MOUNT);
    } else {
        ESP_LOGI(TAG, "no internal FAT store (%s) — using NVS", esp_err_to_name(err));
    }
}

bool locfs_ready(void) {
    return s_ready;
}

uint32_t locfs_free_kb(void) {
    if (!s_ready) return 0;
    uint64_t  total = 0, freeb = 0;
    esp_err_t err = esp_vfs_fat_info(LOCFS_MOUNT, &total, &freeb);
    if (err != ESP_OK) return 0;
    return (uint32_t)(freeb / 1024);
}
