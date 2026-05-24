// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "nodes.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "contacts.h"

static const char *TAG = "nodes";

node_entry_t      node_list[MAX_NODES];
int               node_count  = 0;
int               node_scroll = 0;
int               node_cursor = 0;
SemaphoreHandle_t node_mutex  = NULL;

meshcore_device_role_t node_filter = MESHCORE_DEVICE_ROLE_UNKNOWN;

void nodes_init(void) {
    if (node_mutex == NULL) node_mutex = xSemaphoreCreateMutex();
}

const char *role_label(meshcore_device_role_t role) {
    switch (role) {
        case MESHCORE_DEVICE_ROLE_CHAT_NODE:   return "Chat";
        case MESHCORE_DEVICE_ROLE_REPEATER:    return "Rptr";
        case MESHCORE_DEVICE_ROLE_ROOM_SERVER: return "Room";
        case MESHCORE_DEVICE_ROLE_SENSOR:      return "Sens";
        default:                               return "?";
    }
}

int build_node_display(display_row_t *rows, int max_rows) {
    int n = 0;

    // 1) Contacts first, filtered by node_filter on the stored role.
    for (int ci = 0; ci < contact_count && n < max_rows; ci++) {
        if (node_filter != MESHCORE_DEVICE_ROLE_UNKNOWN &&
            (meshcore_device_role_t)contacts[ci].role != node_filter) continue;
        int ni = -1;
        for (int j = 0; j < MAX_NODES; j++) {
            if (node_list[j].active &&
                memcmp(node_list[j].pub_key, contacts[ci].pub_key, MESHCORE_PUB_KEY_SIZE) == 0) {
                ni = j; break;
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
        if (node_filter != MESHCORE_DEVICE_ROLE_UNKNOWN &&
            node_list[i].role != node_filter) continue;
        if (contact_find(node_list[i].pub_key) >= 0) continue;
        live[live_count++] = i;
    }
    for (int i = 1; i < live_count; i++) {
        int k = live[i], j = i - 1;
        while (j >= 0 && node_list[live[j]].last_seen_ms < node_list[k].last_seen_ms) {
            live[j + 1] = live[j]; j--;
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

void update_node(const meshcore_advert_t *advert, uint32_t now_ms,
                 const lora_packet_stats_t *stats) {
    if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;

    int slot = -1;
    for (int i = 0; i < MAX_NODES; i++) {
        if (node_list[i].active &&
            memcmp(node_list[i].pub_key, advert->pub_key, MESHCORE_PUB_KEY_SIZE) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < MAX_NODES; i++) {
            if (!node_list[i].active) { slot = i; break; }
        }
    }
    // Evict oldest if full.
    if (slot < 0) {
        uint32_t oldest_ms = UINT32_MAX;
        for (int i = 0; i < MAX_NODES; i++) {
            if (node_list[i].last_seen_ms < oldest_ms) {
                oldest_ms = node_list[i].last_seen_ms;
                slot = i;
            }
        }
    }

    if (slot >= 0) {
        node_entry_t *n = &node_list[slot];
        bool is_new = !n->active;
        n->active       = true;
        n->role         = advert->role;
        n->last_seen_ms = now_ms;
        memcpy(n->pub_key, advert->pub_key, MESHCORE_PUB_KEY_SIZE);
        if (!is_new) n->packet_count++;
        else         n->packet_count = 1;
        if (advert->position_valid) {
            n->position_valid = true;
            n->lat = advert->position_lat;
            n->lon = advert->position_lon;
        }

        if (advert->name_valid && advert->name[0] != '\0') {
            strncpy(n->name, advert->name, MESHCORE_MAX_NAME_SIZE);
            n->name[MESHCORE_MAX_NAME_SIZE] = '\0';
        } else if (is_new) {
            // First 4 bytes of pub_key as fallback ID.
            snprintf(n->name, sizeof(n->name), "%02X%02X%02X%02X",
                     advert->pub_key[0], advert->pub_key[1],
                     advert->pub_key[2], advert->pub_key[3]);
        }

        if (stats != NULL && stats->valid) {
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
        ESP_LOGI(TAG, "Node %s (%s) seen — total %d nodes",
                 n->name, role_label(n->role), node_count);
    }

    xSemaphoreGive(node_mutex);
}
