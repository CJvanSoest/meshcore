# Maps

A slippy-map view that renders OSM raster tiles from the SD card with a
live GPS crosshair, scale bar, and node pins. Implemented in v2.5.0.

Open it from the home tile-grid (Map). Navigate with the D-pad / encoder.

## At a glance

| | |
|---|---|
| Tile source | Pre-rendered 256×256 PNGs on `/sd/maps/<profile>/tiles/<z>/<x>/<y>.png` |
| Projection | Web Mercator (the standard slippy-map TMS scheme) |
| Zoom range | 6 → 14 (configurable via `MAP_ZOOM_MIN` / `MAP_ZOOM_MAX` in `components/mc_net/map.h`) |
| Tile cache | LRU, 36 slots in PSRAM (≈ 4.5 MB at 128 KB / RGB565 tile) |
| Tile loader | Background FreeRTOS task, 128-slot xQueue, render task never touches SD |
| Profile slots | Carto enabled by default; Ripple / Cycle / Topo available per the `MAP_PROFILES_ENABLED[]` list — independent directory per style |
| State persisted | Centre lat/lon, zoom, lock toggle, profile (all in NVS) |

## Zoom levels — what is each one good for?

At zoom *z* the world is split into 2^z × 2^z tiles of 256×256 pixels.
The ground resolution depends on latitude; the numbers below are for ~52°N
(the Netherlands) — multiply by `cos(lat)/cos(52°)` for other latitudes.

| Zoom | m / px | Tile covers | Good for | Notes |
|---|---|---|---|---|
| 6 | ~1700 | ~430 km wide | Country overview | A single tile covers most of the Netherlands. |
| 7 | ~850 | ~215 km | Provincial overview | Useful for "where on the map am I roughly?" |
| 8 | ~430 | ~110 km | Region / between cities | Major roads & rivers visible. |
| 9 | ~210 | ~55 km | City-cluster level | Cities show as named blobs. |
| 10 | ~110 | ~27 km | One city + suburbs | Highways labelled, district names appear. |
| 11 | ~53 | ~14 km | City overview | Main roads + parks legible, individual streets thin. |
| 12 | ~27 | ~7 km | Neighbourhood | Street names start to render. |
| 13 | ~13 | ~3.5 km | **Street-level navigation** | All named streets, bus stops, big POIs. |
| 14 | ~7 | ~1.7 km | **Walking / cycling detail** | Footpaths, building outlines, house numbers on busy streets. |
| 15 | ~3 | ~870 m | **Pedestrian path detail** | Individual paths in parks, building footprints. |
| 16 | ~1.5 | ~430 m | **Indoor / micro-navigation** | Crosswalks, footpath alternates, building entrances on dense maps. |
| 17 | ~0.75 | ~220 m | Survey-grade | Mostly overkill on a 720 × 720 screen; useful if you need parcel boundaries. |
| 18 | ~0.4 | ~110 m | Cadastre / specialist | Beyond what raster carto can show usefully. |

### Suggested defaults

| Use case | Recommended zoom |
|---|---|
| Driving / motorbike | 13 |
| Cycling | 14–15 |
| Hiking | 15–16 |
| "I'm standing still, where's the nearest…?" | 16 |
| Showing a friend "this is where the badge is" | 11–12 |

The Tanmatsu's `MAP_ZOOM_MAX` is currently capped at 14. Bumping it to 16
needs (a) tiles rendered at those zooms on the SD, and (b) a one-line
change to `components/mc_net/map.h`. The cache size is independent of max zoom; 36
slots is plenty for a 5 × 5 visible window + ring + history.

## SD card layout

```
/sd/maps/
├── tiles/<z>/<x>/<y>.png        ← MAP_PROFILE_RIPPLE  (legacy europe-6-to-10)
├── carto/tiles/<z>/<x>/<y>.png  ← MAP_PROFILE_CARTO   (the everyday one)
├── cycle/tiles/<z>/<x>/<y>.png  ← MAP_PROFILE_CYCLE
└── topo/tiles/<z>/<x>/<y>.png   ← MAP_PROFILE_TOPO
```

Switch profile under **Settings → Region & Location → Style** (the
`FIELD_MAP_PROFILE` row).
Switching profiles clears the LRU cache, so the first frame after a
switch shows placeholders until tiles come back from SD.

### Enabled styles (Carto-only by default)

The badge ships with **Carto** as the only enabled style, because that's the
single tileset rendered onto the SD card. The Settings "Style" row therefore
shows just `Carto` and the `W`/`S` cycle is a no-op until you enable more.

To offer additional styles (CyclOSM, OpenTopoMap, or the legacy Ripple Europe
tiles), edit the one-line enabled set in
[`components/mc_net/map.c`](../../components/mc_net/map.c):

```c
// Carto-only:
static const map_profile_t MAP_PROFILES_ENABLED[] = {MAP_PROFILE_CARTO};

// e.g. Carto + Cycle + Topo (cycle order follows this list):
static const map_profile_t MAP_PROFILES_ENABLED[] = {
    MAP_PROFILE_CARTO, MAP_PROFILE_CYCLE, MAP_PROFILE_TOPO};
```

The first entry is the power-on default and the value the NVS loader clamps an
unknown stored style to. Each enabled style needs matching tiles copied to
`/sd/maps/<style>/tiles/<z>/<x>/<y>.png` (see the table below) — an enabled
style with no tiles just renders grey. No other code changes are required; the
picker, the default, and the NVS clamp all read this list. (This section is
mirrored to the project wiki "Map styles" page.)

Disk usage scales by 4× per zoom level. For the Netherlands bounding
box (lon 3.10–7.25, lat 50.75–53.70) at one profile:

| Zoom | Tile count | Disk on FAT |
|---|---|---|
| 0 – 13 | 14 189 | ~ 350 MB |
| 14 | 41 769 | ~ 1.3 GB |
| 15 | ~ 170 000 | ~ 4 GB |
| 16 | ~ 670 000 | ~ 15 GB |
| 17 | ~ 2.7 M | ~ 50 GB |

FAT inflates raster PNGs by ~3× compared to ext4 because of 32 KB
cluster waste on tiny sea / empty tiles. Filter those out at render
time if you need to fit four profiles on a 32 GB SD.

## Operation

- **Pan** — D-pad / arrow keys. One press = quarter-tile step.
- **Zoom** — `+` / `-` (or whatever your keymap binds in `input.c`).
- **Lock** — toggled inside the map view itself (persisted to the `map.lock`
  NVS key); there is no Settings row for it.
  When locked the crosshair stays at the GPS fix and the map follows
  you; when unlocked you can pan freely without the next GPS push
  dragging the view away.
- **Profile** — Carto by default; enable Ripple / Cycle / Topo via the one-line
  `MAP_PROFILES_ENABLED[]` list (see "Enabled styles" above), then pick in Settings.

### Node pins

| Role | Shape | Notes |
|---|---|---|
| Chat | Circle | Default for unclassified contacts |
| Repeater | Square | |
| Room | Diamond | |
| Sensor | Triangle | |

Contacts / favourites get a white outline ring around the pin shape.
The pin **nearest to the crosshair** is labelled with the node name
inline so you can identify who you're hovering over.

### Status strip

Top-left of the map shows the active legend; top-right is a status
strip in this order:

```
Z=13   SAT=8   RX=on   BAT=72%
```

- `Z` is the current zoom.
- `SAT` is the live PA1010D satellite count when a GPS is wired in.
- `RX` is the radio receive state.
- `BAT` is battery percentage.

## To be tested

Everything below is implemented and renders correctly on the bench but
hasn't yet been validated in the field for the v2.5.0 release. If you
ride / walk with the badge and hit any of these, an issue with a short
description is hugely appreciated.

| Area | What to verify |
|---|---|
| **Live GPS overlay** | Crosshair lat/lon tracks reality across full GPS profile range (Walking / Cycling / Driving / Manual). Lock-on / lock-off behaviour both behave as described above. |
| **Cycling profile** | 15-second update + 25 m commit-on-move threshold is right for road speeds. Adjust in `Settings → GPS → Tracking` if it feels stale. |
| **Driving profile** | 5-second update + 100 m threshold under highway speeds — no skipped position commits, no NVS thrash. |
| **Walking profile** | 30-second update, 5 m commit threshold, low duty cycle for all-day battery. |
| **z=14 SD load latency** | At z=14 the renderer requests ~25 tiles per pan-step worst case. Loader queue should keep up; if you see grey rectangles persist longer than ~1 s on a fast SD, file an issue with the SD card spec. |
| **Long-session stability** | Background tile-loader task runs continuously; verify no leak / no fragmentation over a multi-hour ride. |
| **GPS lost / regained** | Going under a bridge or into a tunnel should not crash or stall the map; crosshair freezes, recovers on next fix. |
| **Profile switch under load** | Switching style while panning shouldn't deadlock the cache or lose the current view position. |

## Tile rendering — server side

The on-badge tile reader is dumb: it expects pre-rendered 256 × 256 PNGs
laid out at `/sd/maps/<profile>/tiles/<z>/<x>/<y>.png`. Anything that
produces that layout works — pre-baked tile dumps from a vendor,
home-rendered with OpenStreetMap data, or anything in between.

This section describes the local Docker pipeline used to render the
default **Carto** profile.

### Pipeline

```
 Geofabrik PBF ─► osm2pgsql ─► PostgreSQL + PostGIS ─► Mapnik (carto) ─► PNG tile dir
                                       ▲                    ▲
                                       │                    │
                                  35 GB DB          renderd workers
```

All of the above is packaged into the
[`overv/openstreetmap-tile-server`](https://github.com/Overv/openstreetmap-tile-server)
image. It bundles PostgreSQL, PostGIS, osm2pgsql, Mapnik, openstreetmap-carto,
renderd, and Apache `mod_tile` — the same toolchain `tile.openstreetmap.org`
runs.

### Quick start (Docker Compose)

```yaml
version: "3"
services:
  osm-import:
    image: overv/openstreetmap-tile-server:latest
    shm_size: 2gb           # CRITICAL — see gotcha below
    volumes:
      - ./data:/data:ro
      - ./db:/data/database
    command: import
    environment:
      - DOWNLOAD_PBF=https://download.geofabrik.de/europe/netherlands-latest.osm.pbf
      - DOWNLOAD_POLY=https://download.geofabrik.de/europe/netherlands.poly

  osm-server:
    image: overv/openstreetmap-tile-server:latest
    shm_size: 2gb
    volumes:
      - ./db:/data/database
      - ./tiles_out:/var/lib/mod_tile
    command: run
    ports:
      - "8091:80"
    depends_on:
      - osm-import
```

Then a pull script walks the NL bounding box and curls every tile in
the desired zoom range into `/sd/maps/carto/tiles/`. A naive version:

```bash
#!/usr/bin/env bash
set -u
Z_MIN=${Z_MIN:-0}
Z_MAX=${Z_MAX:-14}
LON_MIN=3.10
LON_MAX=7.25
LAT_MIN=50.75
LAT_MAX=53.70
HOST=http://127.0.0.1:8091
OUT=/path/to/sd/maps/carto/tiles

for z in $(seq "$Z_MIN" "$Z_MAX"); do
  # … compute xmin/xmax/ymin/ymax for the bbox at this zoom …
  for x in $(seq "$xmin" "$xmax"); do
    mkdir -p "$OUT/$z/$x"
    for y in $(seq "$ymin" "$ymax"); do
      out_png="$OUT/$z/$x/$y.png"
      [ -s "$out_png" ] && continue          # resume-safe
      curl -fs --max-time 300 -o "$out_png" "$HOST/tile/$z/$x/$y.png" \
        || { rm -f "$out_png"; }
    done
  done
done
```

### Critical gotcha — `shm_size`

Docker defaults `/dev/shm` to **64 MB**. Mapnik's low-zoom (z = 6–8)
tiles query huge geographic regions and PostgreSQL needs **> 1 GB of
shared memory** to build the joined geometry result. Without enough
shm, renderd silently logs

```
Postgis Plugin: ERROR: could not resize shared memory segment
                "/PostgreSQL.XXXXXX": No space left on device
```

and serves 404 for those metatiles — your z=6/7/8 tiles will quietly
not render. Always set `shm_size: 2gb` on both the `import` and `run`
services.

After changing `shm_size`, **`docker compose down && up`** is required
to apply it — `docker compose up --force-recreate` alone does not pick
up the new shm size on every Docker version.

### Hardware requirements

For the Netherlands extract (≈ 1.4 GB PBF, 14 M ways, 200 M nodes):

| Resource | Minimum | Recommended | Notes |
|---|---|---|---|
| **CPU** | 4 cores | 6–8 cores | renderd defaults to 6 worker threads. More cores = more parallel tile renders. No GPU benefit — Mapnik is fully CPU-bound. |
| **RAM** | 8 GB | 16 GB | PostgreSQL `shared_buffers` (~1 GB), osm2pgsql import (4 GB), renderd workers (1–2 GB), `/dev/shm` (2 GB). |
| **Disk (DB)** | 50 GB SSD | 80 GB NVMe | PostGIS DB after NL import is ~ 35 GB. Use SSD — HDD makes tile renders 10× slower at high zoom. |
| **Disk (tiles)** | varies by zoom | see table above | z=0–14 NL ≈ 350 MB, z=15 ≈ 4 GB, z=16 ≈ 15 GB. |
| **GPU** | none | none | Not used by Mapnik. |
| **Network** | 100 Mbit | 1 Gbit | One-time PBF download (~ 1.4 GB), then internal only. |

For larger areas (full European extract ≈ 30 GB PBF), scale linearly:
≈ 700 GB PostGIS DB, 32 GB RAM recommended, multi-day import on
consumer hardware.

### Render-time expectations (NL bbox, 6-core CPU, 16 GB RAM, SSD)

| Zoom | Tile count NL | Wall-clock @ 6 stripes parallel |
|---|---|---|
| 0 – 13 | 14 189 | ~ 30 min |
| 14 | 41 769 | ~ 10 min |
| 15 | ~ 170 000 | ~ 40 min |
| 16 | ~ 670 000 | ~ 3 h |
| 17 | ~ 2.7 M | ~ 15 h |
| 18 | ~ 10.7 M | days |

Higher zooms scale roughly linearly with tile count, but Mapnik gets
slower per-tile at high zoom because each tile contains more feature
data. Don't render z=18 for an area larger than a single city.

### Style profiles other than Carto

`overv/openstreetmap-tile-server` ships openstreetmap-carto. For the
Cycle (CyclOSM) and Topo (OpenTopoMap) profiles you need different
Mapnik style files — either swap the style in the image, or run a
second container with the alternative carto definition mounted in.
Both styles share the same PostGIS DB, so the heavy import only runs
once.

The Ripple profile is a hand-styled coarse Europe set rendered once
externally; it's the fallback when no rendered tiles exist for the
requested area.

## Related
- [GPS sources](GPS-Sources.md) — how the badge knows where it is
- [SD card layout](../reference/SD-Card-Layout.md) — the `/sd/meshcore` side of SD usage
- [Settings / NVS](../reference/Settings-NVS.md) — `map.lat_e6` / `map.lon_e6` / `map.zoom` / `map.lock` / `map.profile` keys
- [Architecture](../architecture/Overview.md) — `map.c` / `render_map.c` / `gps_task.c` modules
