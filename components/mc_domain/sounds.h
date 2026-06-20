// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// Tiny notification-tone module. All tones are synthesised in-app -- no
// embedded WAV/PCM samples, so the binary stays small and the per-event
// "feel" stays fully under our control.
//
// Default event mapping (see sounds.c for the synth recipes):
//   DM      -> Twitter-style 2-note ascending chirp ("tweet")
//   Channel -> short 2-note ascending beep (different timbre from DM)
//   Error   -> single descending tone
//   Boot    -> short major-arpeggio chime
//
// Settings UI lives in a separate "Sounds" category (volume + per-event
// on/off + per-event test action rows). All persistence is in NVS.

#pragma once

#include <stdbool.h>
#include <stdint.h>

// One-shot bring-up. Pairs the BSP audio subsystem (already initialised
// by bsp_device_initialize) with the amplifier + volume that we manage.
// Safe to call multiple times.
void sounds_init(void);

// Apply the stored volume preference (0..100) to the codec. Called by
// sounds_init() and after the volume Settings row commits a change.
void sounds_apply_volume(void);

// Play the four built-in event sounds. Each function honours the
// matching enable flag + global volume, and is safe to call from any
// task; the actual I2S write happens on the calling thread (~150-300 ms
// blocking depending on the recipe). Returns immediately if the event
// is disabled.
void sounds_play_dm(void);
void sounds_play_channel(void);
void sounds_play_error(void);
void sounds_play_boot(void);

// Hard cap on the number of WAV slots surfaced in Settings. Slot 0 is
// reserved for "Off"; slots 1..SOUNDS_MAX_SLOTS index into the alphabetically
// sorted list of .wav files under /sd/meshcore/sounds/.
#define SOUNDS_MAX_SLOTS 20
// Maximum length of a stored WAV basename (without extension). Keeps the
// in-memory list bounded and matches the Settings UI's truncation budget.
#define SOUNDS_NAME_MAX  24

// Rescan /sd/meshcore/sounds/ and rebuild the alphabetical WAV index.
// Safe to call from the UI task; uses readdir, no audio I/O.
void sounds_refresh_list(void);

// Number of WAVs currently discovered (0..SOUNDS_MAX_SLOTS).
int sounds_count(void);

// Filename basename (no path, no .wav) for slot 1..count(). Returns "" for
// slot 0 (Off) or out of range. Pointer is valid until the next refresh.
const char* sounds_slot_name(uint8_t slot);
