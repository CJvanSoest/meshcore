// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "nodes.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "contacts.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "identity.h"

static const char* TAG = "nodes";

node_entry_t      node_list[MAX_NODES];
int               node_count  = 0;
int               node_scroll = 0;
int               node_cursor = 0;
SemaphoreHandle_t node_mutex  = NULL;

meshcore_device_role_t node_filter = MESHCORE_DEVICE_ROLE_UNKNOWN;

// Set by update_node whenever the in-memory list changes; consumed by an
// external save task that calls nodes_save_to_sd() periodically.
static volatile bool s_dirty = false;

// SD layout. Header is fixed-size; records follow back-to-back. Version
// bumps if the record layout changes -- load_from_sd rejects mismatched
// versions so an upgrade doesn't try to interpret old data.
#define NODES_FILE     "/sd/meshcore/nodes.bin"
#define NODES_FILE_TMP "/sd/meshcore/nodes.bin.tmp"
#define NODES_MAGIC    "NODE"
#define NODES_VERSION  1

typedef struct __attribute__((packed)) {
    char     magic[4];  // "NODE"
    uint16_t version;   // NODES_VERSION
    uint16_t count;     // number of records that follow
    uint8_t  reserved[8];
} node_file_hdr_t;

typedef struct __attribute__((packed)) {
    uint8_t  pub_key[MESHCORE_PUB_KEY_SIZE];    // 32
    char     name[MESHCORE_MAX_NAME_SIZE + 1];  // 33
    uint8_t  role;                              // 1
    uint8_t  flags;                             // bit0=position_valid, bit1=stats_valid
    int64_t  last_seen_unix;                    // 8
    int32_t  lat;                               // 4
    int32_t  lon;                               // 4
    uint16_t packet_count;                      // 2
    int8_t   last_rssi_dbm;                     // 1
    int8_t   last_snr_db_x4;                    // 1
} node_record_t;

void nodes_init(void) {
    if (node_mutex == NULL) node_mutex = xSemaphoreCreateMutex();
}

const char* role_label(meshcore_device_role_t role) {
    switch (role) {
        case MESHCORE_DEVICE_ROLE_CHAT_NODE:
            return "Chat";
        case MESHCORE_DEVICE_ROLE_REPEATER:
            return "Rptr";
        case MESHCORE_DEVICE_ROLE_ROOM_SERVER:
            return "Room";
        case MESHCORE_DEVICE_ROLE_SENSOR:
            return "Sens";
        default:
            return "?";
    }
}

int build_node_display(display_row_t* rows, int max_rows) {
    int n = 0;

    // 1) Contacts first, filtered by node_filter on the stored role.
    for (int ci = 0; ci < contact_count && n < max_rows; ci++) {
        if (node_filter != MESHCORE_DEVICE_ROLE_UNKNOWN && (meshcore_device_role_t)contacts[ci].role != node_filter)
            continue;
        int ni = -1;
        for (int j = 0; j < MAX_NODES; j++) {
            if (node_list[j].active && memcmp(node_list[j].pub_key, contacts[ci].pub_key, MESHCORE_PUB_KEY_SIZE) == 0) {
                ni = j;
                break;
            }
        }
        rows[n].is_contact  = true;
        rows[n].contact_idx = ci;
        rows[n].node_idx    = ni;
        n++;
    }

    // 2) Live nodes (not in contacts), filtered by role, sorted by last_seen desc.
    int live[MAX_NODES];
    int live_count = 0;
    for (int i = 0; i < MAX_NODES; i++) {
        if (!node_list[i].active) continue;
        if (node_filter != MESHCORE_DEVICE_ROLE_UNKNOWN && node_list[i].role != node_filter) continue;
        if (contact_find(node_list[i].pub_key) >= 0) continue;
        live[live_count++] = i;
    }
    for (int i = 1; i < live_count; i++) {
        int k = live[i], j = i - 1;
        while (j >= 0 && node_list[live[j]].last_seen_ms < node_list[k].last_seen_ms) {
            live[j + 1] = live[j];
            j--;
        }
        live[j + 1] = k;
    }
    for (int i = 0; i < live_count && n < max_rows; i++) {
        rows[n].is_contact  = false;
        rows[n].contact_idx = -1;
        rows[n].node_idx    = live[i];
        n++;
    }
    return n;
}

void update_node(const meshcore_advert_t* advert, uint32_t now_ms, const lora_packet_stats_t* stats) {
    if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;

    int slot = -1;
    for (int i = 0; i < MAX_NODES; i++) {
        if (node_list[i].active && memcmp(node_list[i].pub_key, advert->pub_key, MESHCORE_PUB_KEY_SIZE) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < MAX_NODES; i++) {
            if (!node_list[i].active) {
                slot = i;
                break;
            }
        }
    }
    // Evict oldest if full. Prefer last_seen_unix when set (consistent
    // across reboots) so a node loaded from SD with an old timestamp gets
    // evicted before a freshly-heard live one. Fall back to last_seen_ms
    // when unix is zero (pre-SNTP-sync entries).
    if (slot < 0) {
        // Evict the least-recently-seen node. Prefer evicting an entry with a
        // real (unix) timestamp; only fall back to a pre-sync entry
        // (last_seen_unix == 0, ranked by boot-relative ms) when no unix-stamped
        // node exists. Tracking the two clocks separately avoids the bug where
        // one zero-unix node sets oldest_unix to 0 and then no later unix-stamped
        // node can ever win the `u < oldest_unix` test.
        int64_t  oldest_unix = INT64_MAX;
        int      unix_slot   = -1;
        uint32_t oldest_ms   = UINT32_MAX;
        int      ms_slot     = -1;
        for (int i = 0; i < MAX_NODES; i++) {
            if (node_list[i].last_seen_unix != 0) {
                if (node_list[i].last_seen_unix < oldest_unix) {
                    oldest_unix = node_list[i].last_seen_unix;
                    unix_slot   = i;
                }
            } else if (node_list[i].last_seen_ms < oldest_ms) {
                oldest_ms = node_list[i].last_seen_ms;
                ms_slot   = i;
            }
        }
        slot = (unix_slot >= 0) ? unix_slot : ms_slot;
    }

    if (slot >= 0) {
        node_entry_t* n      = &node_list[slot];
        // is_new when this slot is being populated for a *different* identity
        // than it currently holds: an empty slot, or one we just evicted. Only a
        // genuine re-hit of the same pubkey is an update. Comparing pubkeys
        // (not n->active) stops an evicted slot from inheriting the previous
        // node's name, packet_count, position and signal stats.
        bool          is_new = memcmp(n->pub_key, advert->pub_key, MESHCORE_PUB_KEY_SIZE) != 0;
        if (is_new) memset(n, 0, sizeof(*n));  // fresh identity takes over the slot
        n->active       = true;
        n->role         = advert->role;
        n->last_seen_ms = now_ms;
        // Tag with absolute time when the system clock has been set; on
        // boot before SNTP/RTC sync we keep 0 and let the next valid hit
        // catch up. Used both for sorting across reboots and the LRU
        // eviction tie-breaker.
        if (identity_sntp_synced()) {
            n->last_seen_unix = (int64_t)time(NULL);
        }
        memcpy(n->pub_key, advert->pub_key, MESHCORE_PUB_KEY_SIZE);
        if (!is_new)
            n->packet_count++;
        else
            n->packet_count = 1;
        if (advert->position_valid) {
            n->position_valid = true;
            n->lat            = advert->position_lat;
            n->lon            = advert->position_lon;
        }

        if (advert->name_valid && advert->name[0] != '\0') {
            strncpy(n->name, advert->name, MESHCORE_MAX_NAME_SIZE);
            n->name[MESHCORE_MAX_NAME_SIZE] = '\0';
        } else if (is_new) {
            // First 4 bytes of pub_key as fallback ID.
            snprintf(n->name, sizeof(n->name), "%02X%02X%02X%02X", advert->pub_key[0], advert->pub_key[1],
                     advert->pub_key[2], advert->pub_key[3]);
        }

        if (stats != NULL) {
            int rssi_dbm = -(int)stats->rssi_pkt_raw / 2;
            if (rssi_dbm < -127) rssi_dbm = -127;
            n->last_rssi_dbm  = (int8_t)rssi_dbm;
            n->last_snr_db_x4 = stats->snr_pkt_raw;
            n->stats_valid    = true;
        }

        node_count = 0;
        for (int i = 0; i < MAX_NODES; i++) {
            if (node_list[i].active) node_count++;
        }
        ESP_LOGI(TAG, "Node %s (%s) seen — total %d nodes", n->name, role_label(n->role), node_count);
        s_dirty = true;
    }

    xSemaphoreGive(node_mutex);
}

// ── SD load / save ───────────────────────────────────────────────────────────
void nodes_save_to_sd(void) {
    if (node_mutex == NULL) return;
    if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return;

    // Write to a temp file then rename, so a power-cut mid-write can't
    // corrupt the canonical file.
    FILE* f = fopen(NODES_FILE_TMP, "wb");
    if (!f) {
        xSemaphoreGive(node_mutex);
        ESP_LOGW(TAG, "nodes_save: fopen failed (SD missing?)");
        return;
    }
    node_file_hdr_t hdr = {0};
    memcpy(hdr.magic, NODES_MAGIC, 4);
    hdr.version = NODES_VERSION;
    hdr.count   = 0;
    // Count active first so the header is accurate.
    for (int i = 0; i < MAX_NODES; i++) {
        if (node_list[i].active) hdr.count++;
    }
    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1) goto fail;

    for (int i = 0; i < MAX_NODES; i++) {
        if (!node_list[i].active) continue;
        node_record_t r = {0};
        memcpy(r.pub_key, node_list[i].pub_key, MESHCORE_PUB_KEY_SIZE);
        strncpy(r.name, node_list[i].name, sizeof(r.name) - 1);
        r.role           = (uint8_t)node_list[i].role;
        r.flags          = (node_list[i].position_valid ? 0x01 : 0) | (node_list[i].stats_valid ? 0x02 : 0);
        r.last_seen_unix = node_list[i].last_seen_unix;
        r.lat            = node_list[i].lat;
        r.lon            = node_list[i].lon;
        r.packet_count   = node_list[i].packet_count;
        r.last_rssi_dbm  = node_list[i].last_rssi_dbm;
        r.last_snr_db_x4 = node_list[i].last_snr_db_x4;
        if (fwrite(&r, sizeof(r), 1, f) != 1) goto fail;
    }
    fclose(f);
    // Atomic-ish swap.
    remove(NODES_FILE);
    if (rename(NODES_FILE_TMP, NODES_FILE) != 0) {
        ESP_LOGW(TAG, "nodes_save: rename failed");
    } else {
        ESP_LOGI(TAG, "nodes saved: %u records", (unsigned)hdr.count);
        s_dirty = false;
    }
    xSemaphoreGive(node_mutex);
    return;

fail:
    fclose(f);
    remove(NODES_FILE_TMP);
    xSemaphoreGive(node_mutex);
    ESP_LOGW(TAG, "nodes_save: write failed (SD full?)");
}

void nodes_load_from_sd(void) {
    if (node_mutex == NULL) return;
    FILE* f = fopen(NODES_FILE, "rb");
    if (!f) {
        ESP_LOGI(TAG, "nodes_load: no saved file (fresh boot)");
        return;
    }
    node_file_hdr_t hdr = {0};
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 || memcmp(hdr.magic, NODES_MAGIC, 4) != 0 || hdr.version != NODES_VERSION) {
        ESP_LOGW(TAG, "nodes_load: bad header (magic/version mismatch)");
        fclose(f);
        return;
    }

    if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        fclose(f);
        return;
    }
    // Wipe whatever's there (should be empty at boot anyway).
    memset(node_list, 0, sizeof(node_list));
    node_count = 0;

    int loaded = 0;
    for (int i = 0; i < hdr.count && i < MAX_NODES; i++) {
        node_record_t r;
        if (fread(&r, sizeof(r), 1, f) != 1) break;
        node_entry_t* n = &node_list[i];
        n->active       = true;
        memcpy(n->pub_key, r.pub_key, MESHCORE_PUB_KEY_SIZE);
        strncpy(n->name, r.name, sizeof(n->name) - 1);
        n->role           = (meshcore_device_role_t)r.role;
        n->last_seen_ms   = 0;  // boot-relative; unknown for a loaded entry
        n->last_seen_unix = r.last_seen_unix;
        n->position_valid = (r.flags & 0x01) != 0;
        n->stats_valid    = (r.flags & 0x02) != 0;
        n->lat            = r.lat;
        n->lon            = r.lon;
        n->packet_count   = r.packet_count;
        n->last_rssi_dbm  = r.last_rssi_dbm;
        n->last_snr_db_x4 = r.last_snr_db_x4;
        loaded++;
    }
    fclose(f);
    node_count = loaded;
    s_dirty    = false;
    xSemaphoreGive(node_mutex);
    ESP_LOGI(TAG, "nodes loaded: %d records", loaded);
}

// Periodic save task: wakes every ~30 s, checks the dirty flag, persists
// if needed. Low priority so it never preempts RX. Single instance.
static void nodes_save_task(void* arg) {
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30 * 1000));
        if (s_dirty) {
            nodes_save_to_sd();
        }
    }
}

void nodes_start_save_task(void) {
    static bool started = false;
    if (started) return;
    started = true;
    // 4 kB stack is plenty; this task only does FILE I/O + memcpy.
    xTaskCreate(nodes_save_task, "nodes_save", 4096, NULL, 1, NULL);
}
