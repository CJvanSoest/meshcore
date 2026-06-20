// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "map.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lodepng.h"
#include "settings_nvs.h"

static const char* TAG = "map";

#define MAP_LAT_LIMIT_DEG 85.05112877980659  // Web-Mercator pole clamp
// Cache size: the visible window is up to 4×4 tiles when the centre pixel
// lies near a tile corner. We also keep a 1-tile ring of off-screen
// neighbours warm (see render_tile_raster) so a single-tile pan finds
// every required tile already decoded. Worst case 5×5 = 25 tiles for the
// current viewport + a ring of headroom for the previous viewport's
// in-view tiles → 36 slots leaves room for that without thrashing.
#define MAP_CACHE_SLOTS   36  // ~4.5 MB PSRAM @ 128 KB/tile

typedef struct {
    bool      used;
    int       zoom;
    int       tile_x;
    int       tile_y;
    uint32_t  last_use_seq;
    pax_buf_t buf;
} tile_cache_entry_t;

static tile_cache_entry_t s_cache[MAP_CACHE_SLOTS] = {0};
static uint32_t           s_seq                    = 0;
// Cache mutex — protects s_cache + s_seq across the render task (read path)
// and the background loader task (write path). The render path takes it
// once around the whole 5×5 raster sweep so newly-arrived tiles can't get
// evicted mid-draw; the loader holds it only while installing a finished
// tile (the slow SD-read + lodepng-decode happens outside the lock).
static SemaphoreHandle_t  s_cache_mutex            = NULL;

// ── Async tile-loader task ──────────────────────────────────────────────────
typedef struct {
    int zoom;
    int tile_x;
    int tile_y;
} tile_req_t;
static QueueHandle_t s_loader_q    = NULL;
static TaskHandle_t  s_loader_task = NULL;

// ── Slippy-map math ─────────────────────────────────────────────────────────

void map_latlon_to_tile(double lat_deg, double lon_deg, int zoom, int* tile_x, int* tile_y, int* px_in_tile,
                        int* py_in_tile) {
    if (lat_deg > MAP_LAT_LIMIT_DEG) lat_deg = MAP_LAT_LIMIT_DEG;
    if (lat_deg < -MAP_LAT_LIMIT_DEG) lat_deg = -MAP_LAT_LIMIT_DEG;

    double n       = (double)(1 << zoom);
    double lat_rad = lat_deg * M_PI / 180.0;
    double xf      = (lon_deg + 180.0) / 360.0 * n;
    double yf      = (1.0 - log(tan(lat_rad) + 1.0 / cos(lat_rad)) / M_PI) / 2.0 * n;

    int xi = (int)floor(xf);
    int yi = (int)floor(yf);
    if (tile_x) *tile_x = xi;
    if (tile_y) *tile_y = yi;
    if (px_in_tile) *px_in_tile = (int)((xf - xi) * MAP_TILE_PX);
    if (py_in_tile) *py_in_tile = (int)((yf - yi) * MAP_TILE_PX);
}

int map_wrap_tile_x(int tile_x, int zoom) {
    int span = 1 << zoom;
    int r    = tile_x % span;
    if (r < 0) r += span;
    return r;
}

// ── Cache management ────────────────────────────────────────────────────────

static int cache_find(int zoom, int tile_x, int tile_y) {
    for (int i = 0; i < MAP_CACHE_SLOTS; i++) {
        if (s_cache[i].used && s_cache[i].zoom == zoom && s_cache[i].tile_x == tile_x && s_cache[i].tile_y == tile_y) {
            return i;
        }
    }
    return -1;
}

static int cache_pick_victim(void) {
    // Prefer an empty slot; otherwise evict the least-recently-used one.
    int      victim = 0;
    uint32_t lru    = UINT32_MAX;
    for (int i = 0; i < MAP_CACHE_SLOTS; i++) {
        if (!s_cache[i].used) return i;
        if (s_cache[i].last_use_seq < lru) {
            lru    = s_cache[i].last_use_seq;
            victim = i;
        }
    }
    if (s_cache[victim].used) {
        pax_buf_destroy(&s_cache[victim].buf);
        s_cache[victim].used = false;
    }
    return victim;
}

// ── View state (centre + zoom + dirty tracking) ─────────────────────────────

int32_t       map_center_lat_e6 = 52080000;  // Den Haag fallback
int32_t       map_center_lon_e6 = 4310000;
uint8_t       map_zoom          = 8;
bool          map_lock_on       = true;  // default per plan §8 decision 4
map_profile_t map_profile       = MAP_PROFILE_RIPPLE;

static bool     s_dirty          = false;
static uint32_t s_last_change_ms = 0;

static uint32_t now_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

void map_state_init(void) {
    int32_t lat, lon;
    uint8_t z;
    if (load_map_state(&lat, &lon, &z)) {
        map_center_lat_e6 = lat;
        map_center_lon_e6 = lon;
        map_zoom          = z;
    }
    if (map_zoom < MAP_ZOOM_MIN) map_zoom = MAP_ZOOM_MIN;
    if (map_zoom > MAP_ZOOM_MAX) map_zoom = MAP_ZOOM_MAX;
    s_dirty          = false;
    s_last_change_ms = now_ms();
}

void map_state_pan(int dx_quarters, int dy_quarters) {
    if (dx_quarters == 0 && dy_quarters == 0) return;
    double lat = (double)map_center_lat_e6 / 1e6;
    double lon = (double)map_center_lon_e6 / 1e6;
    int    tx, ty, px, py;
    map_latlon_to_tile(lat, lon, map_zoom, &tx, &ty, &px, &py);

    double cont_x = (double)tx + (double)px / MAP_TILE_PX + (double)dx_quarters * 0.25;
    double cont_y = (double)ty + (double)py / MAP_TILE_PX + (double)dy_quarters * 0.25;

    double span = (double)(1 << map_zoom);
    while (cont_x < 0.0) cont_x += span;
    while (cont_x >= span) cont_x -= span;
    // Clamp Y to (0, span) so a pole-jump doesn't produce NaN in atan(sinh()).
    const double margin = 0.001;
    if (cont_y < margin) cont_y = margin;
    if (cont_y > span - margin) cont_y = span - margin;

    double new_lon = (cont_x / span) * 360.0 - 180.0;
    double yn      = M_PI * (1.0 - 2.0 * cont_y / span);
    double new_lat = atan(sinh(yn)) * 180.0 / M_PI;

    map_center_lat_e6 = (int32_t)round(new_lat * 1e6);
    map_center_lon_e6 = (int32_t)round(new_lon * 1e6);
    s_dirty           = true;
    s_last_change_ms  = now_ms();
}

void map_state_zoom(int delta) {
    int new_z = (int)map_zoom + delta;
    if (new_z < MAP_ZOOM_MIN) new_z = MAP_ZOOM_MIN;
    if (new_z > MAP_ZOOM_MAX) new_z = MAP_ZOOM_MAX;
    if (new_z == (int)map_zoom) return;
    map_zoom = (uint8_t)new_z;
    // Clear any pending loads from the previous zoom level — they would
    // otherwise crowd out fresh requests for the tiles we actually need
    // right now, and the next render frame will re-enqueue everything
    // that's still missing for the new zoom.
    if (s_loader_q) xQueueReset(s_loader_q);
    s_dirty          = true;
    s_last_change_ms = now_ms();
}

void map_state_tick(void) {
    if (!s_dirty) return;
    if (now_ms() - s_last_change_ms < MAP_STATE_DEBOUNCE_MS) return;
    save_map_state(map_center_lat_e6, map_center_lon_e6, map_zoom);
    s_dirty = false;
}

void map_state_toggle_lock(void) {
    map_lock_on      = !map_lock_on;
    s_dirty          = true;
    s_last_change_ms = now_ms();
}

const char* map_profile_label(map_profile_t p) {
    // Labels mirror the enum names. The actual rendered styling depends on
    // what's been copied into each /sd/maps/<profile>/tiles/ subdir; the
    // generic label ("Carto / Cycle / Topo") tells the user what *use case*
    // the slot is for regardless of which OpenMapTiles style produced the
    // PNGs (OSM Bright vs. CyclOSM vs. OpenTopoMap vs. a fallback).
    switch (p) {
        case MAP_PROFILE_RIPPLE:
            return "Ripple";
        case MAP_PROFILE_CARTO:
            return "Carto";
        case MAP_PROFILE_CYCLE:
            return "Cycle";
        case MAP_PROFILE_TOPO:
            return "Topo";
        default:
            return "?";
    }
}

void map_profile_set(map_profile_t p) {
    if (p >= MAP_PROFILE_COUNT) return;
    if (p == map_profile) return;
    map_profile = p;
    // Drop every cached tile so the next paint reloads from the new sub-dir
    // instead of showing the previous style's PNGs during the transition.
    map_cache_clear();
    s_dirty          = true;
    s_last_change_ms = now_ms();
}

void map_cache_clear(void) {
    if (s_cache_mutex) xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    for (int i = 0; i < MAP_CACHE_SLOTS; i++) {
        if (s_cache[i].used) {
            pax_buf_destroy(&s_cache[i].buf);
            s_cache[i].used = false;
        }
    }
    if (s_cache_mutex) xSemaphoreGive(s_cache_mutex);
}

// ── Tile loader ─────────────────────────────────────────────────────────────

// Inflate PNG bytes into a fresh PAX 16-bit (RGB565) buffer of size 256×256
// (the OSM tile pixel size). Returns true on success; on failure leaves
// `out` uninitialised and the caller must NOT touch it.
static bool decode_png_to_rgb565(const uint8_t* png, size_t png_len, pax_buf_t* out) {
    unsigned char* rgba = NULL;
    unsigned       w    = 0;
    unsigned       h    = 0;
    unsigned       err  = lodepng_decode32(&rgba, &w, &h, png, png_len);
    if (err) {
        ESP_LOGW(TAG, "lodepng decode err=%u", err);
        free(rgba);
        return false;
    }
    if (w != MAP_TILE_PX || h != MAP_TILE_PX) {
        ESP_LOGW(TAG, "unexpected tile size %ux%u", w, h);
        free(rgba);
        return false;
    }
    // Allocate the framebuffer in PSRAM so we don't dent internal RAM with
    // 128 KB per cached tile.
    void* fb_mem = heap_caps_malloc(MAP_TILE_PX * MAP_TILE_PX * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!fb_mem) {
        ESP_LOGW(TAG, "PSRAM alloc failed for tile buffer");
        free(rgba);
        return false;
    }
    pax_buf_init(out, fb_mem, MAP_TILE_PX, MAP_TILE_PX, PAX_BUF_16_565RGB);
    pax_buf_set_orientation(out, PAX_O_UPRIGHT);
    // Per-pixel RGBA8 → RGB565. PAX uses 0xRRRGGGBB encoding internally
    // via pax_col_t (the public API stays AARRGGBB) — easiest to feed it
    // pax_set_pixel(col=ARGB) so we don't depend on internal buffer
    // layout. That costs a function call per pixel but the cache only
    // refills on miss; pan / zoom hit the cached buffer directly.
    for (unsigned y = 0; y < h; y++) {
        for (unsigned x = 0; x < w; x++) {
            unsigned  i   = (y * w + x) * 4;
            pax_col_t col = pax_col_argb(0xFF, rgba[i + 0], rgba[i + 1], rgba[i + 2]);
            pax_set_pixel(out, col, (int)x, (int)y);
        }
    }
    free(rgba);
    return true;
}

// Pick the on-disk tile sub-dir for the active style profile. The default
// profile keeps the historic "/sd/maps/tiles" path so the Ripple zip works
// without renaming; the NAS-rendered profiles use per-style subdirs.
static const char* tile_subdir(void) {
    switch (map_profile) {
        case MAP_PROFILE_CARTO:
            return "carto/tiles";
        case MAP_PROFILE_CYCLE:
            return "cycle/tiles";
        case MAP_PROFILE_TOPO:
            return "topo/tiles";
        case MAP_PROFILE_RIPPLE:
        default:
            return "tiles";
    }
}

static bool load_tile_from_sd(int zoom, int tile_x, int tile_y, pax_buf_t* out) {
    char path[96];
    snprintf(path, sizeof(path), "%s/%s/%d/%d/%d.png", MAP_TILE_ROOT, tile_subdir(), zoom, tile_x, tile_y);
    FILE* f = fopen(path, "rb");
    if (!f) {
        ESP_LOGD(TAG, "miss %s", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 512 * 1024) {
        ESP_LOGW(TAG, "tile %s has weird size %ld", path, n);
        fclose(f);
        return false;
    }
    uint8_t* buf = (uint8_t*)malloc((size_t)n);
    if (!buf) {
        ESP_LOGW(TAG, "alloc failed for tile %s (%ld B)", path, n);
        fclose(f);
        return false;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) {
        ESP_LOGW(TAG, "short read on %s (%zu/%ld)", path, got, n);
        free(buf);
        return false;
    }
    bool ok = decode_png_to_rgb565(buf, (size_t)n, out);
    free(buf);
    return ok;
}

// Lookup-only: returns the cached buf if present, NULL otherwise.
// Misses queue an async load through the background loader task — callers
// can render a placeholder and the tile will be available a few frames
// later without ever blocking the render path on SD I/O + PNG decode.
// Must be called with s_cache_mutex held.
pax_buf_t* map_tile_get(int zoom, int tile_x, int tile_y) {
    if (zoom < MAP_ZOOM_MIN || zoom > MAP_ZOOM_MAX) return NULL;
    int span = 1 << zoom;
    if (tile_y < 0 || tile_y >= span) return NULL;
    tile_x = map_wrap_tile_x(tile_x, zoom);

    int hit = cache_find(zoom, tile_x, tile_y);
    if (hit >= 0) {
        s_cache[hit].last_use_seq = ++s_seq;
        return &s_cache[hit].buf;
    }
    // Cache miss — enqueue an async load. Queue is bounded; if it's full
    // (e.g. user zoomed twice in a row before the loader caught up) we
    // drop the request, the next render frame will re-enqueue it.
    if (s_loader_q) {
        tile_req_t req = {.zoom = zoom, .tile_x = tile_x, .tile_y = tile_y};
        xQueueSend(s_loader_q, &req, 0);
    }
    return NULL;
}

// Render path: acquires the cache mutex for the duration of a single raster
// sweep so the loader can't evict an in-use slot mid-draw.
void map_cache_lock(void) {
    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
}
void map_cache_unlock(void) {
    xSemaphoreGive(s_cache_mutex);
}

// ── Loader task ─────────────────────────────────────────────────────────────

static void loader_task(void* arg) {
    (void)arg;
    while (1) {
        tile_req_t req;
        if (xQueueReceive(s_loader_q, &req, portMAX_DELAY) != pdTRUE) continue;

        // Skip if a peer request already brought the tile in.
        xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
        bool hit = cache_find(req.zoom, req.tile_x, req.tile_y) >= 0;
        xSemaphoreGive(s_cache_mutex);
        if (hit) continue;

        // Slow path: SD read + lodepng decode + RGB565 conversion happen
        // OUTSIDE the cache mutex so the render task isn't blocked.
        pax_buf_t fresh;
        if (!load_tile_from_sd(req.zoom, req.tile_x, req.tile_y, &fresh)) continue;

        // Install — re-check the cache because another loader iteration
        // (or render) may have already added this tile while we decoded.
        xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
        if (cache_find(req.zoom, req.tile_x, req.tile_y) >= 0) {
            xSemaphoreGive(s_cache_mutex);
            pax_buf_destroy(&fresh);
            continue;
        }
        int slot                   = cache_pick_victim();
        s_cache[slot].used         = true;
        s_cache[slot].zoom         = req.zoom;
        s_cache[slot].tile_x       = req.tile_x;
        s_cache[slot].tile_y       = req.tile_y;
        s_cache[slot].buf          = fresh;  // ownership of fb_mem moves here
        s_cache[slot].last_use_seq = ++s_seq;
        xSemaphoreGive(s_cache_mutex);
    }
}

void map_loader_init(void) {
    if (s_cache_mutex) return;
    s_cache_mutex = xSemaphoreCreateMutex();
    // 128-slot queue is enough for a 5×5 sweep at the current zoom + some
    // headroom for the previous viewport's leftover misses (rather than
    // dropping requests and forcing the render frame to re-enqueue).
    s_loader_q    = xQueueCreate(128, sizeof(tile_req_t));
    // 8 KB stack covers lodepng-decode32 of a 256×256 PNG comfortably; the
    // RGBA buffer is heap-allocated. Priority 2 keeps it below the UI but
    // above the housekeeping background tasks.
    xTaskCreate(loader_task, "map_loader", 8192, NULL, 2, &s_loader_task);
}
