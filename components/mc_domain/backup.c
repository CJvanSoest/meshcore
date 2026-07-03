// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "backup.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "backup_codec.h"
#include "channels.h"
#include "contacts.h"
#include "esp_log.h"
#include "history.h"       // history_is_ready
#include "identity.h"      // identity_export_seed / identity_import_seed
#include "settings_nvs.h"  // lora_cfg, gps_*, save_lora_to_nvs, save_gps_coords

#define BACKUP_DIR  "/sd/meshcore"
#define BACKUP_PATH "/sd/meshcore/backup.bin"

static const char* TAG = "backup";

// While true, backup_write() is a no-op. Held during a multi-item restore so we
// don't rewrite the file once per per-item NVS save (channels_add_with_secret /
// contact_ensure each call channels_save_nvs / contacts_save).
static bool s_suspend_write = false;

// Build a codec snapshot from the live tables. Skips Public (slot 0) and any
// inactive channel slot.
static void snapshot(backup_data_t* d) {
    memset(d, 0, sizeof(*d));
    for (int i = 1; i < channel_count && i < CHANNELS_MAX; i++) {
        if (!channels[i].active) continue;
        if (d->n_channels >= BACKUP_MAX_CHANNELS) break;
        backup_channel_t* c = &d->channels[d->n_channels++];
        strncpy(c->name, channels[i].name, BACKUP_CH_NAME_LEN - 1);
        memcpy(c->secret, channels[i].secret, BACKUP_SECRET_LEN);
    }
    for (int i = 0; i < contact_count && i < MAX_CONTACTS; i++) {
        if (d->n_contacts >= BACKUP_MAX_CONTACTS) break;
        backup_contact_t* c = &d->contacts[d->n_contacts++];
        memcpy(c->pub, contacts[i].pub_key, BACKUP_PUB_LEN);
        strncpy(c->alias, contacts[i].alias, BACKUP_ALIAS_LEN - 1);
        c->role = contacts[i].role;
    }

    // v2 tail: identity seed (so the node keeps its key across an NVS wipe),
    // radio config (opaque lora_cfg — restored only if the size still matches,
    // so a struct-layout change can never apply garbage), and saved GPS coords.
    if (identity_export_seed(d->identity_seed)) d->have_identity = 1;
    if (sizeof(lora_cfg) <= BACKUP_RADIO_MAX) {
        memcpy(d->radio, &lora_cfg, sizeof(lora_cfg));
        d->radio_len = (uint8_t)sizeof(lora_cfg);
    }
    d->lat_e6        = gps_lat_e6;
    d->lon_e6        = gps_lon_e6;
    d->have_location = gps_position_valid ? 1 : 0;
}

static bool write_file(const char* path, const backup_data_t* d) {
    uint8_t buf[BACKUP_MAX_BYTES];
    size_t  n = backup_encode(d, buf, sizeof(buf));
    if (n == 0) return false;
    mkdir(BACKUP_DIR, 0775);  // ignores EEXIST
    FILE* f = fopen(path, "wb");
    if (!f) {
        ESP_LOGW(TAG, "fopen(%s) failed", path);
        return false;
    }
    size_t w = fwrite(buf, 1, n, f);
    fclose(f);
    return w == n;
}

bool backup_write(void) {
    if (!history_is_ready() || s_suspend_write) return false;
    backup_data_t d;
    snapshot(&d);
    bool ok = write_file(BACKUP_PATH, &d);
    if (ok) ESP_LOGI(TAG, "mirror updated: %d channel(s), %d contact(s)", d.n_channels, d.n_contacts);
    return ok;
}

static bool read_path(const char* path, backup_data_t* out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    uint8_t buf[BACKUP_MAX_BYTES];
    size_t  n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    return backup_decode(buf, n, out) == BACKUP_OK;
}

// Newest /sd/meshcore/backup-<unix>.bin (the timestamped copies factory reset
// leaves behind). Fills out+path, returns its size, or -1 if none.
static long newest_snapshot(char* out, size_t cap) {
    DIR* dir = opendir(BACKUP_DIR);
    if (!dir) return -1;
    char           best[300] = {0};
    long           best_sz   = -1;
    time_t         best_mt   = 0;
    struct dirent* e;
    while ((e = readdir(dir)) != NULL) {
        if (strncmp(e->d_name, "backup-", 7) != 0) continue;
        size_t L = strlen(e->d_name);
        if (L < 5 || L > 64 || strcmp(e->d_name + L - 4, ".bin") != 0) continue;
        char p[300];  // sized for BACKUP_DIR + '/' + a full dirent name (keeps -Wformat-truncation happy)
        snprintf(p, sizeof(p), "%s/%s", BACKUP_DIR, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (best[0] == 0 || st.st_mtime >= best_mt) {
            best_mt = st.st_mtime;
            best_sz = (long)st.st_size;
            strncpy(best, p, sizeof(best) - 1);
        }
    }
    closedir(dir);
    if (best[0] == 0) return -1;
    if (out && cap) {
        strncpy(out, best, cap - 1);
        out[cap - 1] = '\0';
    }
    return best_sz;
}

bool backup_exists(void) {
    struct stat st;
    if (stat(BACKUP_PATH, &st) == 0 && st.st_size > 0) return true;
    return newest_snapshot(NULL, 0) > 0;  // a post-factory-reset snapshot still counts
}

long backup_file_size(void) {
    struct stat st;
    if (stat(BACKUP_PATH, &st) == 0 && st.st_size > 0) return (long)st.st_size;
    return newest_snapshot(NULL, 0);  // -1 if none
}

long backup_file_mtime(void) {
    struct stat st;
    if (stat(BACKUP_PATH, &st) == 0 && st.st_size > 0) return (long)st.st_mtime;
    char snap[300];
    if (newest_snapshot(snap, sizeof(snap)) > 0 && stat(snap, &st) == 0) return (long)st.st_mtime;
    return -1;
}

int backup_restore(bool force) {
    if (!history_is_ready()) return -1;
    backup_data_t d;
    bool          have = read_path(BACKUP_PATH, &d);
    // Manual restore falls back to the newest timestamped snapshot (e.g. after a
    // factory reset, which removes the canonical mirror). Boot auto-restore
    // stays canonical-only so a reset is never silently undone.
    if (!have && force) {
        char snap[80];
        if (newest_snapshot(snap, sizeof(snap)) > 0) have = read_path(snap, &d);
    }
    if (!have) return -1;

    // Auto path: only restore into an apparently-wiped NVS so we never
    // resurrect channels/contacts the user deliberately removed (those deletes
    // already rewrote the mirror to match).
    if (!force) {
        bool wiped = (channel_count <= 1) && (contact_count == 0);
        if (!wiped) return 0;
        if (d.n_channels == 0 && d.n_contacts == 0) return 0;
    }

    int applied     = 0;
    s_suspend_write = true;  // per-item NVS saves must not thrash the file
    for (int i = 0; i < d.n_channels; i++) {
        if (channels_add_with_secret(d.channels[i].name, d.channels[i].secret) >= 0) applied++;
    }
    for (int i = 0; i < d.n_contacts; i++) {
        if (contact_ensure(d.contacts[i].pub, d.contacts[i].alias, d.contacts[i].role) == 1) applied++;
    }
    s_suspend_write = false;

    // v2 tail: restore identity, radio config and GPS coords when present.
    if (d.have_identity) {
        if (identity_import_seed(d.identity_seed)) applied++;
    }
    if (d.radio_len == sizeof(lora_cfg)) {
        memcpy(&lora_cfg, d.radio, sizeof(lora_cfg));
        save_lora_to_nvs();
        applied++;
    }
    if (d.have_location) {
        gps_lat_e6         = d.lat_e6;
        gps_lon_e6         = d.lon_e6;
        gps_position_valid = true;
        save_gps_coords();
        applied++;
    }

    ESP_LOGI(TAG, "restore(force=%d): %d item(s) applied", (int)force, applied);
    return applied;
}

bool backup_factory_reset(void) {
    if (!history_is_ready()) return false;

    // 1. Timestamped safety copy the user can restore from by hand.
    backup_data_t d;
    snapshot(&d);
    char ts_path[64];
    snprintf(ts_path, sizeof(ts_path), "%s/backup-%lu.bin", BACKUP_DIR, (unsigned long)time(NULL));
    write_file(ts_path, &d);

    // 2. Clear all user channels + contacts from NVS. Suspend the mirror write
    //    so the canonical file isn't recreated between the clear and the remove.
    s_suspend_write = true;
    for (int i = CHANNELS_MAX - 1; i >= 1; i--) {
        if (channels[i].active) channels_remove(i);
    }
    contact_count = 0;
    memset(contacts, 0, sizeof(contact_t) * MAX_CONTACTS);
    memset(contact_unread, 0, sizeof(int) * MAX_CONTACTS);
    contacts_save();
    s_suspend_write = false;

    // 3. Drop the canonical mirror so the next boot's auto-restore stays quiet.
    remove(BACKUP_PATH);
    ESP_LOGW(TAG, "factory reset done; safety copy at %s", ts_path);
    return true;
}
