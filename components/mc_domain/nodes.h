// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
// SPDX-FileContributor: Ilias el Matani <hello@ilias.codes>

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "lora.h"  // lora_packet_stats_t
#include "meshcore/packet.h"
#include "meshcore/payload/advert.h"

// Discovered-nodes capacity. 20 was fine for a few-hour session, but with
// SD-backed persistence we now keep a longer history so the user can come
// back after a reboot and still see / promote nodes they saw earlier.
// 200 × node_entry_t (~96 bytes incl. padding) ≈ 19 KB RAM — comfortable.
#define MAX_NODES 200

typedef struct {
    bool                   active;
    uint8_t                pub_key[MESHCORE_PUB_KEY_SIZE];
    char                   name[MESHCORE_MAX_NAME_SIZE + 1];
    meshcore_device_role_t role;
    uint32_t               last_seen_ms;     // boot-relative; for runtime "age" display
    int64_t                last_seen_unix;   // absolute; for cross-reboot LRU + display
    uint16_t               packet_count;
    bool                   position_valid;
    int32_t                lat;   // degrees × 1e6 (MeshCore upstream scale)
    int32_t                lon;
    bool                   stats_valid;
    int8_t                 last_rssi_dbm;
    int8_t                 last_snr_db_x4;
} node_entry_t;

extern node_entry_t      node_list[MAX_NODES];
extern int               node_count;
extern int               node_scroll;
extern int               node_cursor;
extern SemaphoreHandle_t node_mutex;

// node_filter == MESHCORE_DEVICE_ROLE_UNKNOWN means "show all roles"
extern meshcore_device_role_t node_filter;

// Combined display row for the Nodes tab: contacts on top (possibly offline),
// then live nodes that aren't already in contacts.
typedef struct {
    bool is_contact;
    int  contact_idx;    // index into contacts[]; -1 if live-only
    int  node_idx;       // index into node_list[]; -1 if offline contact
} display_row_t;

// Caller must hold node_mutex. Returns the number of rows written.
int build_node_display(display_row_t *rows, int max_rows);

// "Chat" / "Rptr" / "Room" / "Sens" / "?"
const char *role_label(meshcore_device_role_t role);

// Allocate the node_mutex. Call once before any RX task touches node_list.
void nodes_init(void);

// Upsert an advert into node_list. Caller passes optional packet stats (RSSI/SNR).
void update_node(const meshcore_advert_t *advert, uint32_t now_ms,
                 const lora_packet_stats_t *stats);

// ── SD persistence ───────────────────────────────────────────────────────────
// Discovered nodes survive an app restart so the user can come back and
// promote a previously-seen node to favorite. Storage: /sd/meshcore/nodes.bin
// with a small header (magic "NODE" + version + count) and packed records.
// LRU eviction in update_node already uses last_seen_ms; for nodes loaded
// from SD we also have last_seen_unix so cross-reboot ordering stays stable.
void nodes_load_from_sd(void);   // call once at boot after SD mount; no-op if SD unavailable
void nodes_save_to_sd(void);     // explicit save; usually triggered by the dirty-flag task
// Spin up a low-priority FreeRTOS task that wakes every ~30 s and saves
// when the internal dirty flag is set. Safe to call once after the SD is mounted.
void nodes_start_save_task(void);
