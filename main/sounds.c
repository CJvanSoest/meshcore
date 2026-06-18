// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "sounds.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "bsp/audio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"

#include "settings_nvs.h"

static const char *TAG = "sounds";

#define SOUND_DIR     "/sd/meshcore/sounds"
#define CHUNK_SAMPLES 256                  // PCM samples per i2s_channel_write batch
#define DEFAULT_SR    22050u

static i2s_chan_handle_t s_i2s    = NULL;
static bool              s_ready  = false;
static uint32_t          s_rate   = 0;
// Single mutex so two parallel preview taps can't trample one another's
// I2S writes. If a sound is already playing we just drop the second one.
static SemaphoreHandle_t s_busy   = NULL;

// In-memory index of the .wav files under SOUND_DIR. Built by
// sounds_refresh_list(), consumed by the Settings UI for label display +
// by play_async() to resolve a slot to a real filename. Names are stored
// without the .wav extension so the UI can show them as-is.
static char s_names[SOUNDS_MAX_SLOTS][SOUNDS_NAME_MAX + 1];
static int  s_count = 0;

// ── WAV parsing ──────────────────────────────────────────────────────────────
typedef struct __attribute__((packed)) {
    char     id[4];
    uint32_t size;
} chunk_hdr_t;

typedef struct __attribute__((packed)) {
    uint16_t format;          // 1 = PCM
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
} fmt_chunk_t;

static bool wav_open(const char *path, FILE **out_f,
                     uint32_t *out_rate, uint16_t *out_channels,
                     uint32_t *out_data_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    char riff[12];
    if (fread(riff, 1, 12, f) != 12 ||
        memcmp(riff, "RIFF", 4) != 0 ||
        memcmp(riff + 8, "WAVE", 4) != 0) {
        ESP_LOGW(TAG, "%s: not a RIFF/WAVE file", path);
        fclose(f);
        return false;
    }
    bool have_fmt = false, have_data = false;
    fmt_chunk_t fmt = {0};
    uint32_t data_size = 0;
    while (!(have_fmt && have_data)) {
        chunk_hdr_t ch;
        if (fread(&ch, sizeof(ch), 1, f) != 1) break;
        if (memcmp(ch.id, "fmt ", 4) == 0) {
            size_t n = ch.size < sizeof(fmt) ? ch.size : sizeof(fmt);
            if (fread(&fmt, 1, n, f) != n) break;
            if (ch.size > n) fseek(f, (long)(ch.size - n), SEEK_CUR);
            have_fmt = true;
        } else if (memcmp(ch.id, "data", 4) == 0) {
            data_size = ch.size;
            have_data = true;
            break;
        } else {
            uint32_t skip = ch.size + (ch.size & 1);
            if (fseek(f, (long)skip, SEEK_CUR) != 0) break;
        }
    }
    if (!have_fmt || !have_data || fmt.format != 1 || fmt.bits_per_sample != 16) {
        ESP_LOGW(TAG, "%s: unsupported WAV (fmt=%u bits=%u)",
                 path, fmt.format, fmt.bits_per_sample);
        fclose(f);
        return false;
    }
    *out_f         = f;
    *out_rate      = fmt.sample_rate;
    *out_channels  = fmt.channels;
    *out_data_size = data_size;
    return true;
}

// Stream samples to I2S + drain tail. Heap-allocated buffers keep the task
// stack flat -- a 4 KB chunk on stack reliably trampled the FreeRTOS guard.
static void stream_wav(FILE *f, uint16_t channels, uint32_t data_size) {
    if (channels != 1 && channels != 2) {
        ESP_LOGW(TAG, "unsupported channel count %u", channels);
        return;
    }
    int16_t *in_buf  = (int16_t *)malloc(CHUNK_SAMPLES * sizeof(int16_t));
    int16_t *out_buf = (int16_t *)malloc(CHUNK_SAMPLES * 2 * sizeof(int16_t));
    if (!in_buf || !out_buf) {
        free(in_buf); free(out_buf);
        ESP_LOGW(TAG, "oom: skipping sound");
        return;
    }
    uint32_t bytes_left = data_size;
    uint16_t in_stride  = channels;
    while (bytes_left > 0) {
        uint32_t want_frames = CHUNK_SAMPLES / in_stride;
        if (want_frames * in_stride * 2 > bytes_left) {
            want_frames = bytes_left / (in_stride * 2);
        }
        if (want_frames == 0) break;
        size_t got = fread(in_buf, 2, want_frames * in_stride, f);
        if (got == 0) break;
        bytes_left -= got * 2;

        size_t out_frames = got / in_stride;
        for (size_t i = 0; i < out_frames; i++) {
            int16_t l = in_buf[i * in_stride];
            int16_t r = (in_stride == 2) ? in_buf[i * in_stride + 1] : l;
            out_buf[i * 2]     = l;
            out_buf[i * 2 + 1] = r;
        }
        size_t written = 0;
        i2s_channel_write(s_i2s, out_buf, out_frames * 2 * sizeof(int16_t),
                          &written, pdMS_TO_TICKS(500));
    }
    // Tail of silence so the DMA buffer flushes -- otherwise the codec
    // will keep emitting whatever was last in its FIFO until the next
    // playback starts.
    memset(out_buf, 0, CHUNK_SAMPLES * 2 * sizeof(int16_t));
    size_t tail_frames = s_rate / 12;
    while (tail_frames > 0) {
        size_t batch = tail_frames > CHUNK_SAMPLES ? CHUNK_SAMPLES : tail_frames;
        size_t written = 0;
        i2s_channel_write(s_i2s, out_buf, batch * 2 * sizeof(int16_t),
                          &written, pdMS_TO_TICKS(200));
        tail_frames -= batch;
    }
    free(in_buf);
    free(out_buf);
}

// ── Async playback task ──────────────────────────────────────────────────────
// UI rows fire sounds_play_*() which spawns this task. The UI task returns
// immediately so the watchdog stays happy and edit_mode toggles feel
// instant; the heavy SDMMC + I2S blocking happens off the UI.
typedef struct {
    char path[64];
} play_arg_t;

static void play_task(void *arg) {
    play_arg_t *pa = (play_arg_t *)arg;

    if (s_busy && xSemaphoreTake(s_busy, 0) != pdTRUE) {
        // Already playing something; drop the request.
        free(pa);
        vTaskDelete(NULL);
        return;
    }

    FILE    *f = NULL;
    uint32_t rate, data_size;
    uint16_t channels;
    if (wav_open(pa->path, &f, &rate, &channels, &data_size)) {
        if (rate != s_rate && bsp_audio_set_rate(rate) == ESP_OK) s_rate = rate;
        ESP_LOGI(TAG, "play %s (%uHz %uch %u B)",
                 pa->path, (unsigned)rate, (unsigned)channels, (unsigned)data_size);
        stream_wav(f, channels, data_size);
        fclose(f);
    }

    if (s_busy) xSemaphoreGive(s_busy);
    free(pa);
    vTaskDelete(NULL);
}

static void play_async(uint8_t slot) {
    if (!s_ready || slot == 0 || slot > SOUNDS_MAX_SLOTS) return;
    if (slot > s_count) return;
    play_arg_t *pa = (play_arg_t *)malloc(sizeof(play_arg_t));
    if (!pa) return;
    snprintf(pa->path, sizeof(pa->path), "%s/%s.wav", SOUND_DIR, s_names[slot - 1]);
    // 8 KB stack: WAV parse uses ~100B locals, I2S write is shallow, fread
    // through SDMMC ~1 KB. Headroom for any deeper SD call.
    if (xTaskCreate(play_task, "sound_play", 8192, pa, 4, NULL) != pdPASS) {
        free(pa);
    }
}

// ── WAV directory index ─────────────────────────────────────────────────────
// Strips the trailing ".wav"/".WAV" from a directory entry and copies the
// remaining basename into dst (truncated to SOUNDS_NAME_MAX). Returns true
// if the entry was a WAV (case-insensitive) and a non-empty basename remained.
static bool sounds_take_wav_name(const char *fname, char *dst) {
    size_t len = strlen(fname);
    if (len < 5) return false;
    const char *ext = fname + len - 4;
    if ((ext[0] != '.') ||
        (ext[1] != 'w' && ext[1] != 'W') ||
        (ext[2] != 'a' && ext[2] != 'A') ||
        (ext[3] != 'v' && ext[3] != 'V')) {
        return false;
    }
    size_t base = len - 4;
    if (base == 0)                    return false;
    if (base > SOUNDS_NAME_MAX)       base = SOUNDS_NAME_MAX;
    memcpy(dst, fname, base);
    dst[base] = '\0';
    return true;
}

static int sounds_name_cmp(const void *a, const void *b) {
    const char *sa = (const char *)a;
    const char *sb = (const char *)b;
    return strcasecmp(sa, sb);
}

void sounds_refresh_list(void) {
    s_count = 0;
    memset(s_names, 0, sizeof(s_names));
    DIR *d = opendir(SOUND_DIR);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && s_count < SOUNDS_MAX_SLOTS) {
        if (e->d_name[0] == '.') continue;
        char tmp[SOUNDS_NAME_MAX + 1];
        if (sounds_take_wav_name(e->d_name, tmp)) {
            memcpy(s_names[s_count], tmp, SOUNDS_NAME_MAX + 1);
            s_count++;
        }
    }
    closedir(d);
    if (s_count > 1) qsort(s_names, s_count, sizeof(s_names[0]), sounds_name_cmp);
    ESP_LOGI(TAG, "sound list: %d WAV(s) under %s", s_count, SOUND_DIR);
}

int sounds_count(void) { return s_count; }

const char *sounds_slot_name(uint8_t slot) {
    if (slot == 0 || slot > s_count) return "";
    return s_names[slot - 1];
}

// ── Public API ───────────────────────────────────────────────────────────────
void sounds_apply_volume(void) {
    if (!s_ready) return;
    float pct = (float)sound_volume_pct;
    if (pct < 0.0f)   pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    bsp_audio_set_volume(pct);
}

void sounds_init(void) {
    if (s_ready) return;
    esp_err_t r = bsp_audio_get_i2s_handle(&s_i2s);
    if (r != ESP_OK || s_i2s == NULL) {
        ESP_LOGW(TAG, "i2s handle unavailable (rc=%d) -- sounds disabled", r);
        return;
    }
    bsp_audio_set_rate(DEFAULT_SR);
    s_rate = DEFAULT_SR;
    bsp_audio_set_amplifier(true);
    s_busy  = xSemaphoreCreateMutex();
    s_ready = true;
    sounds_apply_volume();
    mkdir("/sd/meshcore",        0755);
    mkdir("/sd/meshcore/sounds", 0755);
    sounds_refresh_list();
    ESP_LOGI(TAG, "sounds ready (vol=%u%%)", sound_volume_pct);
}

void sounds_play_dm(void)      { if (sound_volume_pct) play_async(sound_dm_slot);      }
void sounds_play_channel(void) { if (sound_volume_pct) play_async(sound_channel_slot); }
void sounds_play_error(void)   { if (sound_volume_pct) play_async(sound_error_slot);   }
void sounds_play_boot(void)    { if (sound_volume_pct) play_async(sound_boot_slot);    }
