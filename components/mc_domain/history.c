// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "history.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "app_config.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "history_trim.h"
#include "locfs.h"
#include "mbedtls/aes.h"
#include "mbedtls/md.h"
#include "sdmmc_cmd.h"

// Tanmatsu µSD pins — slot 0 (slot 1 = hosted Wi-Fi link).
#define MC_SDCARD_CLK 43
#define MC_SDCARD_CMD 44
#define MC_SDCARD_D0  39
#define MC_SDCARD_D1  40
#define MC_SDCARD_D2  41
#define MC_SDCARD_D3  42

#define SD_MOUNT_POINT    "/sd"
#define HISTORY_REC_MAGIC "MCR1"

// Fallback caps applied ONLY when running on the internal FAT store (locfd),
// which is small and shared with the launcher's apps/icons. On an SD card
// these are effectively disabled (SIZE_CAP_NONE) since the card is large.
#define SIZE_CAP_NONE     UINT32_MAX
#define INTERNAL_DM_CAP   (32u * 1024u)  // per-DM log ceiling on internal store
#define INTERNAL_CH_CAP   (48u * 1024u)  // per-channel log ceiling on internal store
#define INTERNAL_FLOOR_KB 512u           // stop appending when free space drops below this

static const char* TAG = "history";

static bool              s_ready       = false;
static const char*       s_status      = "off";
static uint8_t           s_key[32]     = {0};
static SemaphoreHandle_t s_mutex       = NULL;
static sdmmc_card_t*     s_card        = NULL;
static bool              s_on_internal = false;  // true when using locfd, not SD
static bool              s_floor_hit   = false;  // rate-limit the "store full" warning

// Base directory + the dm/ subdir built once at init, so the same code path
// serves SD ("/sd/meshcore") and the internal FAT store ("/locfd/meshcore").
static char s_root[32]   = {0};
static char s_dm_dir[40] = {0};
static char s_ch_dir[40] = {0};

typedef struct __attribute__((packed)) {
    uint8_t  magic[4];   // "MCR1"
    uint16_t plain_len;  // 1..MAX_MSG_TEXT
    uint8_t  flags;      // bit0 = is_mine
    uint8_t  reserved;
    uint32_t ts_unix;  // seconds since epoch (LE)
    uint8_t  iv[16];
} history_rec_hdr_t;  // 28 bytes
_Static_assert(sizeof(history_rec_hdr_t) == HISTORY_REC_HDR_SIZE, "history header size drift");

static esp_err_t mount_sd(void) {
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot         = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot  = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk                  = MC_SDCARD_CLK;
    slot.cmd                  = MC_SDCARD_CMD;
    slot.d0                   = MC_SDCARD_D0;
    slot.d1                   = MC_SDCARD_D1;
    slot.d2                   = MC_SDCARD_D2;
    slot.d3                   = MC_SDCARD_D3;
    slot.width                = 4;
    slot.flags               |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mnt = {
        .format_if_mount_failed = false,
        .max_files              = 4,
        .allocation_unit_size   = 16 * 1024,
    };
    return esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mnt, &s_card);
}

// Point the log root at `base`, deriving the dm/ and ch/ subdirs. `internal`
// selects the fallback caps (small shared partition) vs the SD path (no caps).
static void set_root(const char* base, bool internal) {
    snprintf(s_root, sizeof(s_root), "%s", base);
    snprintf(s_dm_dir, sizeof(s_dm_dir), "%s/dm", base);
    snprintf(s_ch_dir, sizeof(s_ch_dir), "%s/ch", base);
    s_on_internal = internal;
}

void history_init(const uint8_t prv_key[32]) {
    s_mutex = xSemaphoreCreateMutex();

    esp_err_t e = mount_sd();
    if (e == ESP_OK) {
        ESP_LOGI(TAG, "SD mounted at %s", SD_MOUNT_POINT);
        set_root(SD_MOUNT_POINT "/meshcore", false);
        s_status = "ok";
    } else if (locfs_ready()) {
        // No card: keep DMs, channel logs and the node list across reboots on
        // the always-present internal FAT store, bounded so we don't crowd out
        // the launcher's apps/icons on the shared partition (issue #85).
        ESP_LOGW(TAG, "SD mount failed (%s) — chat history on internal FAT store", esp_err_to_name(e));
        set_root(LOCFS_MOUNT "/meshcore", true);
        s_status = "int";
    } else {
        ESP_LOGW(TAG, "no SD and no internal FAT store — chat history disabled");
        s_status = (e == ESP_ERR_NOT_FOUND || e == ESP_ERR_TIMEOUT) ? "no-sd" : "err";
        return;
    }

    mkdir(s_root, 0775);  // ignores EEXIST
    mkdir(s_dm_dir, 0775);
    mkdir(s_ch_dir, 0775);

    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), prv_key, 32, (const uint8_t*)"mc-history-v1", 13,
                    s_key);

    s_ready = true;
}

const char* history_root(void) {
    return s_ready ? s_root : NULL;
}

bool history_on_internal(void) {
    return s_ready && s_on_internal;
}

const char* history_status(void) {
    return s_status;
}
bool history_is_ready(void) {
    return s_ready;
}

uint64_t history_sd_capacity_bytes(void) {
    if (!s_ready || s_card == NULL) return 0;
    return (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
}

// Drop the oldest records from `path` until it fits within `cap`. Walks record
// headers to find the first byte offset whose retained tail (the newest
// records) is <= cap, then rewrites from that offset via a temp + rename.
// No-op when the cap is disabled or the file already fits. Caller holds s_mutex.
static void trim_file_to_cap(const char* path, uint32_t cap) {
    if (cap == SIZE_CAP_NONE) return;

    // Size the log from the open handle rather than a separate stat() on the
    // path, so there is no check-then-use gap on the filename (CodeQL TOCTOU).
    FILE* f = fopen(path, "rb");
    if (!f) return;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return;
    }
    long total_l = ftell(f);
    if (total_l <= (long)cap) {
        fclose(f);
        return;
    }
    uint32_t total = (uint32_t)total_l;

    long threshold = (long)total - (long)cap;  // first boundary >= this fits the cap
    long cut       = 0;                        // byte offset to keep from
    long here      = 0;
    while (1) {
        history_rec_hdr_t hdr;
        if (fseek(f, here, SEEK_SET) != 0) break;
        if (fread(&hdr, sizeof(hdr), 1, f) != 1) break;
        if (memcmp(hdr.magic, HISTORY_REC_MAGIC, 4) != 0 || hdr.plain_len == 0 || hdr.plain_len > MAX_MSG_TEXT)
            break;  // corrupt tail — leave the file for load_impl's self-heal
        long next = here + (long)history_rec_disk_size(hdr.plain_len);
        cut       = here;                // keep-from candidate (also handles "keep newest")
        if (here >= threshold) break;    // retaining from here already fits the cap
        if (next >= (long)total) break;  // last record — keep it even if oversized
        here = next;
    }
    if (cut <= 0) {
        fclose(f);  // nothing to drop (single oversized record, or empty)
        return;
    }

    char tmp[80];
    snprintf(tmp, sizeof(tmp), "%s.t", path);
    FILE* g = fopen(tmp, "wb");
    if (!g) {
        fclose(f);
        return;
    }
    fseek(f, cut, SEEK_SET);
    uint8_t buf[512];
    size_t  n;
    bool    ok = true;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (fwrite(buf, 1, n, g) != n) {
            ok = false;
            break;
        }
    }
    if (fclose(g) != 0) ok = false;
    fclose(f);
    if (ok) {
        remove(path);
        if (rename(tmp, path) != 0)
            ESP_LOGW(TAG, "trim(%s): rename failed", path);
        else
            ESP_LOGI(TAG, "trim(%s): kept newest %u KB", path, (unsigned)((total - (uint32_t)cut) / 1024));
    } else {
        remove(tmp);
        ESP_LOGW(TAG, "trim(%s): rewrite failed", path);
    }
}

static void append_impl(const char* path, const char* text, bool is_mine, uint32_t cap) {
    if (!s_ready || text == NULL) return;
    int N = (int)strnlen(text, MAX_MSG_TEXT);
    if (N <= 0) return;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return;

    // On the small shared internal partition, stop appending before we starve
    // the launcher's app/icon space. SD is large enough to skip this.
    if (s_on_internal && locfs_free_kb() < INTERNAL_FLOOR_KB) {
        if (!s_floor_hit) {
            ESP_LOGW(TAG, "internal store < %u KB free — pausing history appends", INTERNAL_FLOOR_KB);
            s_floor_hit = true;
        }
        xSemaphoreGive(s_mutex);
        return;
    }
    s_floor_hit = false;

    history_rec_hdr_t hdr;
    memcpy(hdr.magic, HISTORY_REC_MAGIC, 4);
    hdr.plain_len = (uint16_t)N;
    hdr.flags     = is_mine ? 0x01 : 0x00;
    hdr.reserved  = 0;
    hdr.ts_unix   = (uint32_t)time(NULL);
    esp_fill_random(hdr.iv, 16);

    int     padded = ((N + 16) / 16) * 16;
    uint8_t pt[MAX_MSG_TEXT + 32];
    memcpy(pt, text, N);
    uint8_t pad = (uint8_t)(padded - N);
    for (int i = N; i < padded; i++) pt[i] = pad;

    uint8_t             ct[MAX_MSG_TEXT + 32];
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, s_key, 128);
    uint8_t iv_copy[16];
    memcpy(iv_copy, hdr.iv, 16);
    mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, padded, iv_copy, pt, ct);
    mbedtls_aes_free(&aes);

    // A half-written record makes load_impl read its leftovers as the next
    // header, which stops the load for good. Roll back to this length instead.
    struct stat pre;
    long        start = (stat(path, &pre) == 0) ? (long)pre.st_size : -1;

    FILE* f = fopen(path, "ab");
    if (f) {
        bool ok = fwrite(&hdr, sizeof(hdr), 1, f) == 1 && fwrite(ct, padded, 1, f) == 1;
        if (fclose(f) != 0) ok = false;
        if (!ok) {
            ESP_LOGW(TAG, "append(%s): short write, rolling back", path);
            if (start >= 0) truncate(path, start);
        }
    } else {
        ESP_LOGW(TAG, "append: fopen(%s) failed", path);
    }
    trim_file_to_cap(path, cap);
    xSemaphoreGive(s_mutex);
}

static void load_impl(const char* path, history_ring_add_fn add) {
    if (!s_ready || add == NULL) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return;

    FILE* f = fopen(path, "rb");
    if (!f) {
        xSemaphoreGive(s_mutex);
        return;
    }

    int  loaded   = 0;
    bool fatal    = false;
    long good_end = 0;  // offset just past the last record that parsed cleanly
    while (1) {
        history_rec_hdr_t hdr;
        if (fread(&hdr, sizeof(hdr), 1, f) != 1) break;
        if (memcmp(hdr.magic, HISTORY_REC_MAGIC, 4) != 0) {
            ESP_LOGW(TAG, "(%s): bad magic at rec %d — stop", path, loaded);
            fatal = true;
            break;
        }
        if (hdr.plain_len == 0 || hdr.plain_len > MAX_MSG_TEXT) {
            ESP_LOGW(TAG, "(%s): bad len %u at rec %d — stop", path, hdr.plain_len, loaded);
            fatal = true;
            break;
        }
        int     padded = ((hdr.plain_len + 16) / 16) * 16;
        uint8_t ct[MAX_MSG_TEXT + 32];
        if (fread(ct, padded, 1, f) != 1) break;

        uint8_t             pt[MAX_MSG_TEXT + 32];
        mbedtls_aes_context aes;
        mbedtls_aes_init(&aes);
        mbedtls_aes_setkey_dec(&aes, s_key, 128);
        mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, padded, hdr.iv, ct, pt);
        mbedtls_aes_free(&aes);

        uint8_t pad = pt[padded - 1];
        if (pad == 0 || pad > 16 || (padded - pad) != hdr.plain_len) {
            ESP_LOGW(TAG, "(%s): decrypt mismatch at rec %d — stop", path, loaded);
            fatal = true;
            break;
        }
        int N = hdr.plain_len;

        char text[MAX_MSG_TEXT + 1];
        memcpy(text, pt, N);
        text[N] = '\0';

        add(text, (hdr.flags & 0x01) != 0);
        loaded++;
        good_end = ftell(f);
    }
    fclose(f);

    // Self-heal: a file that cannot be parsed at all (likely written with a
    // previous identity key) blocks all future loads. Wipe it so the next
    // append starts a fresh log under the current key.
    if (fatal && loaded == 0) {
        ESP_LOGW(TAG, "(%s): unreadable from start — removing stale file", path);
        remove(path);
    } else if (fatal && good_end > 0) {
        // Left in place, the bad record would stop every future load too.
        ESP_LOGW(TAG, "(%s): keeping %d good record(s), dropping the tail", path, loaded);
        truncate(path, good_end);
    }

    ESP_LOGI(TAG, "load(%s): %d record(s) restored", path, loaded);
    xSemaphoreGive(s_mutex);
}

// Per-channel path: /sd/meshcore/ch/<secret-hex8>.bin
// 8-byte secret prefix as hex = 16 chars, unique enough for max 8 channels.
static void ch_path(const uint8_t secret[16], char* out, size_t cap) {
    snprintf(out, cap, "%s/%02x%02x%02x%02x%02x%02x%02x%02x.bin", s_ch_dir, secret[0], secret[1], secret[2], secret[3],
             secret[4], secret[5], secret[6], secret[7]);
}

void history_append_channel(const uint8_t secret[16], const char* text, bool is_mine) {
    if (!s_ready || secret == NULL) return;
    char path[64];
    ch_path(secret, path, sizeof(path));
    append_impl(path, text, is_mine, s_on_internal ? INTERNAL_CH_CAP : SIZE_CAP_NONE);
}

void history_load_channel(const uint8_t secret[16], history_ring_add_fn add) {
    if (!s_ready || secret == NULL) return;
    char path[64];
    ch_path(secret, path, sizeof(path));
    load_impl(path, add);
}

void history_delete_channel(const uint8_t secret[16]) {
    if (!s_ready || secret == NULL) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return;
    char path[64];
    ch_path(secret, path, sizeof(path));
    remove(path);
    xSemaphoreGive(s_mutex);
}

// Per-contact path: /sd/meshcore/dm/<pubkey-hex16>.bin
// 8-byte pubkey prefix as hex = 16 chars, unique enough for max 16 contacts.
static void dm_path(const uint8_t pub[32], char* out, size_t cap) {
    snprintf(out, cap, "%s/%02x%02x%02x%02x%02x%02x%02x%02x.bin", s_dm_dir, pub[0], pub[1], pub[2], pub[3], pub[4],
             pub[5], pub[6], pub[7]);
}

void history_append_dm(const uint8_t peer_pub[32], const char* text, bool is_mine) {
    if (!s_ready || peer_pub == NULL) return;
    char path[64];
    dm_path(peer_pub, path, sizeof(path));
    append_impl(path, text, is_mine, s_on_internal ? INTERNAL_DM_CAP : SIZE_CAP_NONE);
}

void history_load_dm(const uint8_t peer_pub[32], history_ring_add_fn add) {
    if (!s_ready || peer_pub == NULL) return;
    char path[64];
    dm_path(peer_pub, path, sizeof(path));
    load_impl(path, add);
}

void history_delete_dm(const uint8_t peer_pub[32]) {
    if (!s_ready || peer_pub == NULL) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return;
    char path[64];
    dm_path(peer_pub, path, sizeof(path));
    remove(path);
    xSemaphoreGive(s_mutex);
}

// ── Storage Viewer enumeration (issue #70) ──────────────────────────────────

// Parse a "<16-hex>.bin" log filename back into its 8-byte prefix. Returns false
// for anything that isn't exactly that shape (skips ".", "..", stray files).
static bool parse_hex8(const char* name, uint8_t out[8]) {
    if (strnlen(name, 32) != 20) return false;  // 16 hex + ".bin"
    for (int i = 0; i < 16; i++) {
        char c = name[i];
        int  v;
        if (c >= '0' && c <= '9')
            v = c - '0';
        else if (c >= 'a' && c <= 'f')
            v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            v = c - 'A' + 10;
        else
            return false;
        if (i & 1)
            out[i / 2] |= (uint8_t)v;
        else
            out[i / 2] = (uint8_t)(v << 4);
    }
    return strcmp(name + 16, ".bin") == 0;
}

// Rebuild a log path from an 8-byte prefix. Branch on kind so the format string
// takes a literal directory (keeps -Wformat-truncation quiet on a fixed buffer).
static void prefix_path(const uint8_t id[8], bool is_dm, char* out, size_t cap) {
    if (is_dm)
        snprintf(out, cap, "%s/%02x%02x%02x%02x%02x%02x%02x%02x.bin", s_dm_dir, id[0], id[1], id[2], id[3], id[4],
                 id[5], id[6], id[7]);
    else
        snprintf(out, cap, "%s/%02x%02x%02x%02x%02x%02x%02x%02x.bin", s_ch_dir, id[0], id[1], id[2], id[3], id[4],
                 id[5], id[6], id[7]);
}

static int scan_dir(const char* dir, bool is_dm, history_conv_t* out, int max, int n, uint64_t* total) {
    DIR* d = opendir(dir);
    if (!d) return n;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        uint8_t id[8];
        if (!parse_hex8(e->d_name, id)) continue;
        char p[300];  // dir + '/' + a full dirent name (keeps -Wformat-truncation happy)
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (total) *total += (uint64_t)st.st_size;
        if (out && n < max) {
            out[n].is_dm = is_dm;
            memcpy(out[n].id, id, 8);
            out[n].bytes = (uint32_t)st.st_size;
            n++;
        }
    }
    closedir(d);
    return n;
}

int history_list_conversations(history_conv_t* out, int max, uint64_t* out_total) {
    if (out_total) *out_total = 0;
    if (!s_ready) return 0;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(300)) != pdTRUE) return 0;
    int n = 0;
    n     = scan_dir(s_dm_dir, true, out, max, n, out_total);
    n     = scan_dir(s_ch_dir, false, out, max, n, out_total);
    if (out) {  // selection sort by size desc (n is tiny — max 31 logs)
        for (int i = 0; i < n; i++)
            for (int j = i + 1; j < n; j++)
                if (out[j].bytes > out[i].bytes) {
                    history_conv_t t = out[i];
                    out[i]           = out[j];
                    out[j]           = t;
                }
    }
    xSemaphoreGive(s_mutex);
    return n;
}

bool history_delete_conversation(const uint8_t id[8], bool is_dm) {
    if (!s_ready || id == NULL) return false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return false;
    char path[64];
    prefix_path(id, is_dm, path, sizeof(path));
    bool ok = (remove(path) == 0);
    xSemaphoreGive(s_mutex);
    return ok;
}

int history_clear_all(void) {
    if (!s_ready) return 0;
    int removed = 0;
    for (int pass = 0; pass < 2; pass++) {
        bool        is_dm = (pass == 0);
        const char* dir   = is_dm ? s_dm_dir : s_ch_dir;
        // Snapshot the ids first — removing entries while walking the dir is
        // unsafe on FAT. 40 comfortably covers the 15 channels + 16 contacts max.
        uint8_t     ids[40][8];
        int         cnt = 0;
        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(300)) != pdTRUE) return removed;
        DIR* d = opendir(dir);
        if (d) {
            struct dirent* e;
            while ((e = readdir(d)) != NULL && cnt < 40)
                if (parse_hex8(e->d_name, ids[cnt])) cnt++;
            closedir(d);
        }
        xSemaphoreGive(s_mutex);
        for (int i = 0; i < cnt; i++)
            if (history_delete_conversation(ids[i], is_dm)) removed++;
    }
    return removed;
}
