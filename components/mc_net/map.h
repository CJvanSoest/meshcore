// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
// SPDX-FileContributor: Ilias el Matani <hello@ilias.codes>
//
// Slippy-map tile pipeline for VIEW_MAP. Slippy-map math converts a
// (lat, lon, zoom) triple into a tile index + sub-tile offset; the tile
// loader fetches PNGs from /sd/maps/tiles/{z}/{x}/{y}.png and keeps a
// small RGB565 LRU cache in PSRAM so panning doesn't re-decode every
// tile every frame.

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "pax_gfx.h"

// map_profile_t + the map_profile / map_lock_on globals and the
// map_profile_label helper live in the neutral config_types.h so the L1
// settings store can read them without depending on this module.
#include "config_types.h"

// SD root of the map data. The active style profile (see map_profile_t in
// config_types.h) chooses which subfolder under here is read on tile miss:
//   Ripple     → /sd/maps/tiles/{z}/{x}/{y}.png        (legacy, unchanged)
//   OSM-Bright → /sd/maps/carto/tiles/{z}/{x}/{y}.png  (NAS-rendered)
//   CyclOSM    → /sd/maps/cycle/tiles/{z}/{x}/{y}.png
//   OpenTopo   → /sd/maps/topo/tiles/{z}/{x}/{y}.png
#define MAP_TILE_ROOT "/sd/maps"
#define MAP_TILE_PX   256

// Switch the active profile. Clears the tile cache so old-style PNGs don't
// linger across the change, marks state dirty so the debounced NVS save
// catches it. The enabled-style set + cycle/default helpers are declared in
// config_types.h (neutral header) and defined in map.c.
void map_profile_set(map_profile_t p);

// Zoom bounds. MIN matches the Ripple Europe tileset's lower edge; MAX is
// bumped to 17 (building scale) so a later self-hosted tilemaker render
// of NL z=11-17 drops in without firmware changes. Tiles missing in the
// current SD content render as a grey rect with a corner grid line, so
// over-zooming is harmless until the deeper tiles are present.
#define MAP_ZOOM_MIN 6
#define MAP_ZOOM_MAX 17

// Slippy-map: (lat°, lon°, zoom) → tile (x, y) + pixel offset inside the
// tile. py_in_tile uses the standard Web-Mercator Y so 0 = north edge.
// lat is clamped to ±85.0511° (the Mercator pole limit) before mapping.
void map_latlon_to_tile(double lat_deg, double lon_deg, int zoom, int* tile_x, int* tile_y, int* px_in_tile,
                        int* py_in_tile);

// Wraps `tile_x` modulo 2^zoom so panning across the antimeridian works.
int map_wrap_tile_x(int tile_x, int zoom);

// Look up (or load + decode) a tile from /sd/maps/tiles/{z}/{x}/{y}.png.
// Returns a pointer to a PAX RGB565 buffer of size MAP_TILE_PX×MAP_TILE_PX
// on success, NULL if the tile is missing, malformed, or out of bounds.
// The pointer is owned by the cache and must NOT be freed by the caller;
// it stays valid until the cache evicts that slot.
pax_buf_t* map_tile_get(int zoom, int tile_x, int tile_y);

// Drop every cached tile (e.g. on view exit or low-memory pressure).
void map_cache_clear(void);

// Spawn the background tile-loader task + cache mutex + request queue.
// Idempotent; call once at boot before VIEW_MAP can be opened.
void map_loader_init(void);

// Render-side lock around a single raster sweep. While held, the loader
// cannot evict an in-use slot mid-pax_draw_image. Mutex pair, both safe
// from the same task.
void map_cache_lock(void);
void map_cache_unlock(void);

// ── View-state (centre + zoom) ──────────────────────────────────────────────
// VIEW_MAP's centre + zoom live here so input handlers, the render path, and
// NVS persistence all see the same single source of truth. All three are
// loaded from NVS (or sensible defaults) on boot via map_state_init().
extern int32_t map_center_lat_e6;  // 1e-6 degrees
extern int32_t map_center_lon_e6;
extern uint8_t map_zoom;  // clamped to [MAP_ZOOM_MIN, MAP_ZOOM_MAX]

// Initialise centre + zoom from NVS, falling back to Den Haag at zoom 8 if
// the keys are missing. Idempotent; call once at boot before rendering.
void map_state_init(void);

// Pan by N quarter-tiles in either axis. Positive dx = east, positive dy =
// south (map-paper convention). Wraps in x (longitude) and clamps y so the
// centre stays inside the Web-Mercator projection range.
void map_state_pan(int dx_quarters, int dy_quarters);

// Adjust the zoom by ±1. No-ops at the [MIN, MAX] edge.
void map_state_zoom(int delta);

// Called from the render loop. Persists state to NVS if it changed at least
// `MAP_STATE_DEBOUNCE_MS` ago — so panning at speed doesn't write per key
// press, but a brief pause commits to flash.
#define MAP_STATE_DEBOUNCE_MS 2000
void map_state_tick(void);

// Lock-to-position: when on, render_map() snaps the centre to the latest
// live GPS fix before drawing. Toggled by the 'L' key inside the map view.
// (map_lock_on is declared in config_types.h so settings_nvs can persist it.)
void map_state_toggle_lock(void);
