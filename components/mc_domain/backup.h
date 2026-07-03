// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// On-SD safety mirror of the channel list + DM inbox. NVS stays the primary
// store (always present, loads before the SD mounts), but a shared 16 KB NVS
// partition can fill and get blanket-erased at boot — which permanently loses
// private-channel secrets (random keys that live nowhere else). This module
// keeps a copy on the SD card so a wipe is survivable.
//
// The backup is intentionally NOT encrypted with the identity key: that key
// also lives in NVS, so it would be gone after the very wipe this protects
// against. v1 stores plaintext on the card; encrypting with a chip-unique eFuse
// key (survives NVS erase) is future hardening.

#pragma once

#include <stdbool.h>

// Snapshot the live channels[] (skipping Public) + contacts[] to
// /sd/meshcore/backup.bin. No-op returning false if the SD card is not ready.
// Called automatically whenever channels_save_nvs() / contacts_save() run, so
// the mirror always tracks NVS.
bool backup_write(void);

// Restore channels + contacts from the SD backup into NVS.
//   force == false : auto path — only restores when NVS looks wiped (just the
//                    Public channel + zero contacts). Avoids resurrecting items
//                    the user deliberately deleted. Returns 0 when it skips.
//   force == true  : manual path (Toolbox) — always merges the backup in.
// Returns the number of items applied, or -1 on error / no backup.
int backup_restore(bool force);

// True if a non-empty /sd/meshcore/backup.bin exists.
bool backup_exists(void);

// Size in bytes of the canonical backup file, or -1 if none.
long backup_file_size(void);

// Modification time (unix seconds) of the canonical backup file, or of the
// newest timestamped snapshot if the canonical one is gone; -1 if none.
long backup_file_mtime(void);

// Factory reset that keeps a safety copy: writes a timestamped
// /sd/meshcore/backup-<unix>.bin, clears all user channels + contacts from NVS,
// and removes the canonical backup so the next boot does not auto-restore.
// Does NOT reboot — the caller decides. Returns false if the SD is not ready.
bool backup_factory_reset(void);
