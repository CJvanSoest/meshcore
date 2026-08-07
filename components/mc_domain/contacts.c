// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "contacts.h"
#include <stdio.h>
#include <string.h>
#include "atomic_file.h"
#include "backup.h"  // backup_write — mirror contact changes to SD
#include "esp_log.h"
#include "locfs.h"  // internal FAT store (keeps contacts off the shared NVS)
#include "nvs.h"

#define NVS_CONTACTS_BLOB "mc.contacts"
#define CT_FILE           LOCFS_MOUNT "/mc_contacts.bin"

static const char* TAG = "contacts";

contact_t contacts[MAX_CONTACTS];
int       contact_count                = 0;
int       contact_unread[MAX_CONTACTS] = {0};

void contacts_load(void) {
    contact_count = 0;
    memset(contacts, 0, sizeof(contacts));

    // Prefer the internal FAT store; fall back to (and migrate off) NVS. Same
    // scheme as channels — see locfs.c / issue #66.
    if (locfs_ready()) {
        FILE* f = fopen(CT_FILE, "rb");
        if (f) {
            size_t n = fread(contacts, 1, sizeof(contacts), f);
            fclose(f);
            contact_count = (int)(n / sizeof(contact_t));
            if (contact_count > MAX_CONTACTS) contact_count = MAX_CONTACTS;
            ESP_LOGI(TAG, "Loaded %d contact(s) from %s", contact_count, CT_FILE);
            return;
        }
    }

    nvs_handle_t handle;
    if (nvs_open("system", NVS_READONLY, &handle) != ESP_OK) return;
    size_t blob_sz = sizeof(contacts);
    if (nvs_get_blob(handle, NVS_CONTACTS_BLOB, contacts, &blob_sz) == ESP_OK) {
        int n = (int)(blob_sz / sizeof(contact_t));
        if (n > MAX_CONTACTS) n = MAX_CONTACTS;
        contact_count = n;
        ESP_LOGI(TAG, "Loaded %d contact(s) from NVS", contact_count);
    }
    nvs_close(handle);

    if (locfs_ready() && contact_count > 0) {
        ESP_LOGI(TAG, "migrating contacts NVS -> %s", CT_FILE);
        contacts_save();  // writes the file + erases the NVS key
    }
}

void contacts_save(void) {
    if (locfs_ready()) {
        if (!atomic_write_file(CT_FILE, contacts, (size_t)contact_count * sizeof(contact_t))) {
            ESP_LOGE(TAG, "contacts write failed, keeping the NVS copy");
            return;
        }
        nvs_handle_t handle;  // drop the legacy NVS copy only now the file is safe
        if (nvs_open("system", NVS_READWRITE, &handle) == ESP_OK) {
            nvs_erase_key(handle, NVS_CONTACTS_BLOB);  // ignores not-found
            nvs_commit(handle);
            nvs_close(handle);
        }
        ESP_LOGI(TAG, "Saved %d contact(s) to %s", contact_count, CT_FILE);
        backup_write();
        return;
    }

    nvs_handle_t handle;
    if (nvs_open("system", NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "NVS open for contacts write failed");
        return;
    }
    if (contact_count == 0) {
        nvs_erase_key(handle, NVS_CONTACTS_BLOB);
    } else {
        nvs_set_blob(handle, NVS_CONTACTS_BLOB, contacts, (size_t)contact_count * sizeof(contact_t));
    }
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "Saved %d contact(s) to NVS", contact_count);
    backup_write();  // keep the SD mirror in sync (no-op if SD not ready)
}

int contact_find(const uint8_t* pub) {
    for (int i = 0; i < contact_count; i++) {
        if (memcmp(contacts[i].pub_key, pub, MESHCORE_PUB_KEY_SIZE) == 0) return i;
    }
    return -1;
}

int contact_find_by_prefix(const uint8_t prefix[8]) {
    for (int i = 0; i < contact_count; i++) {
        if (memcmp(contacts[i].pub_key, prefix, 8) == 0) return i;
    }
    return -1;
}

// Idempotent add — used to persist anyone we've ever DM'd with.
// Returns 1 if added, 0 if already known, -1 if full.
int contact_ensure(const uint8_t* pub, const char* name, uint8_t role) {
    if (contact_find(pub) >= 0) return 0;
    if (contact_count >= MAX_CONTACTS) return -1;
    int        slot = contact_count;
    contact_t* c    = &contacts[contact_count++];
    memcpy(c->pub_key, pub, MESHCORE_PUB_KEY_SIZE);
    strncpy(c->alias, name ? name : "", CONTACT_ALIAS_LEN - 1);
    c->alias[CONTACT_ALIAS_LEN - 1] = '\0';
    c->role                         = role;
    c->flags                        = 0;
    contact_unread[slot]            = 0;
    contacts_save();
    return 1;
}

// Add (uses node name as alias) or remove. Returns +1 added, 0 removed, -1 full.
int contact_toggle(const uint8_t* pub, const char* name, uint8_t role) {
    int idx = contact_find(pub);
    if (idx >= 0) {
        for (int i = idx; i < contact_count - 1; i++) {
            contacts[i]       = contacts[i + 1];
            contact_unread[i] = contact_unread[i + 1];
        }
        contact_count--;
        memset(&contacts[contact_count], 0, sizeof(contact_t));
        contact_unread[contact_count] = 0;
        contacts_save();
        return 0;
    }
    if (contact_count >= MAX_CONTACTS) return -1;
    int        slot = contact_count;
    contact_t* c    = &contacts[contact_count++];
    memcpy(c->pub_key, pub, MESHCORE_PUB_KEY_SIZE);
    strncpy(c->alias, name ? name : "", CONTACT_ALIAS_LEN - 1);
    c->alias[CONTACT_ALIAS_LEN - 1] = '\0';
    c->role                         = role;
    c->flags                        = 0;
    contact_unread[slot]            = 0;
    contacts_save();
    return 1;
}

void contact_mark_unread(const uint8_t* pub) {
    int idx = contact_find(pub);
    if (idx >= 0) contact_unread[idx]++;
}

void contact_clear_unread(const uint8_t* pub) {
    int idx = contact_find(pub);
    if (idx >= 0) contact_unread[idx] = 0;
}

int contact_unread_total(void) {
    int sum = 0;
    for (int i = 0; i < contact_count; i++) sum += contact_unread[i];
    return sum;
}
