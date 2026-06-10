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
