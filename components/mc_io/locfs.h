// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// Optional internal-flash FAT store, mounted from the "locfd" data partition
// present in the Tanmatsu partition table (~3.9 MB). Used to keep the channel
// list + DM inbox OFF the tiny shared NVS partition, shrinking our NVS
// footprint (see the backup/data-loss work in #66). Unlike the SD card this is
// always present, so mesh data survives an NVS wipe without needing a card.
//
// Safety: we mount WITHOUT format_if_mount_failed. If the partition is absent,
// blank, or already owned by the firmware, the mount simply fails and callers
// transparently fall back to NVS — we never format a partition we don't own.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define LOCFS_MOUNT "/locfd"

// Try to mount the "locfd" partition at LOCFS_MOUNT. Call once early in boot
// (after nvs_flash_init, before channels_init / contacts_load). Idempotent.
void locfs_init(void);

// True if the internal FAT store mounted and is usable as a file backend.
bool locfs_ready(void);

// Free space on the internal FAT store, in kibibytes. Returns 0 when the store
// is not mounted or the query fails. Used to gate appends before the shared
// partition (also holding the launcher's apps/icons) runs out of room.
uint32_t locfs_free_kb(void);
