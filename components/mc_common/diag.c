// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "diag.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"

static diag_entry_t     *s_ring  = NULL;
static int               s_head  = 0;  // next write slot
static int               s_count = 0;  // valid entries (<= DIAG_LOG_SIZE)
static volatile uint32_t s_total = 0;  // lifetime captures (benign unlocked read)
static SemaphoreHandle_t s_mutex = NULL;

void diag_init(void) {
    if (s_mutex == NULL) s_mutex = xSemaphoreCreateMutex();
    if (s_ring == NULL) {
        size_t bytes = sizeof(diag_entry_t) * DIAG_LOG_SIZE;
        s_ring = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
        if (s_ring == NULL) s_ring = malloc(bytes);  // fall back to internal RAM
    }
}

void diag_capture(uint8_t dir, const uint8_t *frame, uint8_t len,
                  int8_t rssi_dbm, int8_t snr_db_x4) {
    if (s_ring == NULL || s_mutex == NULL || frame == NULL || len == 0) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return;

    diag_entry_t *e = &s_ring[s_head];
    e->now_ms     = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    e->dir        = dir;
    e->rssi_dbm   = rssi_dbm;
    e->snr_db_x4  = snr_db_x4;
    e->full_len   = len;
    uint8_t n     = len > DIAG_RAW_MAX ? DIAG_RAW_MAX : len;
    e->raw_len    = n;
    memcpy(e->raw, frame, n);

    s_head = (s_head + 1) % DIAG_LOG_SIZE;
    if (s_count < DIAG_LOG_SIZE) s_count++;
    s_total++;

    xSemaphoreGive(s_mutex);
}

int diag_snapshot(diag_entry_t out[DIAG_LOG_SIZE], int *out_head) {
    if (s_ring == NULL || s_mutex == NULL || out == NULL) return 0;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return 0;

    // Hold the lock only for one bulk copy + head/count read, so a concurrent
    // capture waits microseconds rather than the length of a per-entry loop and
    // does not get starved into dropping under burst RX. The caller derives the
    // newest-first order from the write head, lock-free.
    memcpy(out, s_ring, sizeof(diag_entry_t) * DIAG_LOG_SIZE);
    int head  = s_head;
    int count = s_count;

    xSemaphoreGive(s_mutex);
    if (out_head) *out_head = head;
    return count;
}

uint32_t diag_total(void) { return s_total; }

void diag_clear(void) {
    if (s_mutex == NULL) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
    s_head  = 0;
    s_count = 0;
    xSemaphoreGive(s_mutex);
}
