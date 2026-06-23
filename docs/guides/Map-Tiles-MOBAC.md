# Getting map tiles onto the SD card (no Docker required)

The MeshCore map view only reads pre-rendered PNG tiles from the SD card — it
never renders anything itself. If you don't want to stand up the
[Docker render pipeline](../features/Maps.md#tile-rendering--server-side), you
can download ready-made tiles with the free desktop tool
**MOBAC (Mobile Atlas Creator)** and copy them across.

This produces exactly the layout the badge expects:
`/sd/maps/<profile>/tiles/<z>/<x>/<y>.png` (256 × 256 PNG, standard XYZ
slippy scheme). See [Maps → SD card layout](../features/Maps.md#sd-card-layout).

> **Shipped firmware enables only the `Carto` style**, so put your tiles in
> `maps/carto/tiles/` or the map renders grey. To enable Cycle / Topo / Ripple,
> see [Maps → Enabled styles](../features/Maps.md#enabled-styles-carto-only-by-default).

## 1. Install MOBAC

1. Download MOBAC from <https://mobac.sourceforge.io> — free, runs on
   Windows / macOS / Linux, needs Java.
2. Unzip and start it (`Mobile Atlas Creator.exe`, or `start.sh` / the `.jar`
   on macOS/Linux).

## 2. Pick the map style

In the **Map Source** dropdown (top-left) choose **OpenStreetMap Mapnik**. This
is the standard OSM "carto" style and matches the badge's default **Carto**
profile.

> CyclOSM / OpenTopoMap sources exist in MOBAC too, but those profiles are not
> switched on in the default firmware — use OpenStreetMap Mapnik unless you have
> enabled another style yourself.

## 3. Select your area and zoom levels

1. Pan/zoom the MOBAC map to your region and drag a **selection rectangle** over
   the area you want offline.
2. In **Zoom Levels**, tick **6, 7, 8, 9, 10, 11, 12, 13, 14**.
   - The badge caps zoom at **14** (`MAP_ZOOM_MAX`), so don't download higher —
     those tiles will never be shown.
   - Lower zooms (6–10) are tiny; the bulk of the size is z13–14.
3. Rough size guide for a whole country (e.g. Netherlands), one style:
   z6–13 ≈ 350 MB, adding z14 ≈ 1.3 GB total. A single city is a few MB.

## 4. Create the atlas in the right format

1. In **Atlas Settings** (top-right) set the format to **Osmdroid ZIP**. This
   produces tiles named exactly `<z>/<x>/<y>.png` in the same XYZ scheme the
   badge uses.
2. Name the atlas, click **Add selection**, then **Atlas → Create atlas**.
3. When it finishes, find the output `.zip` and **unzip** it. You get a tree
   like:
   ```
   <name>/
     6/32/21.png
     7/64/42.png
     13/4231/2701.png
     ...
   ```

> Be polite to the free OSM servers: download modest areas, not whole continents
> at z14. For large regions use a key-based source (Thunderforest / MapTiler)
> as a custom MOBAC map source instead.

## 5. Put the tiles on the SD card with the right names

The badge expects this **exact path** (note the `tiles` sub-folder):

```
/sd/maps/carto/tiles/<z>/<x>/<y>.png
```

Create `maps/carto/tiles/` on the card and copy the **contents** of the
unzipped atlas (the `6/`, `7/`, … `14/` folders) into it:

```
SD root
└── maps
    └── carto
        └── tiles
            ├── 6/32/21.png
            ├── 7/...
            └── 14/...
```

Other styles use the same shape: `maps/cycle/tiles/...`,
`maps/topo/tiles/...`. The legacy Ripple style uses `maps/tiles/...` (no style
sub-folder).

## 6. Copying tips (lots of small files)

A tile set is **tens of thousands of tiny files**, so a plain drag-and-drop can
be slow:

- **macOS:** macOS sprinkles hidden `._*` and `.DS_Store` files onto FAT cards,
  which waste space and can confuse readers. Clean them up after copying:
  ```bash
  find /Volumes/<YOUR_SD>/maps -name '._*' -delete
  find /Volumes/<YOUR_SD>/maps -name '.DS_Store' -delete
  ```
  For very large sets, building a FAT image and writing it with `dd` is far
  faster than a per-file copy.
- **Windows / Linux:** drag-and-drop is fine; let it finish fully and eject
  safely.

## 7. Use it on the badge

1. Insert the SD card, power on, open **Map** from the home grid.
2. Pan with the D-pad / encoder, zoom with `+` / `-`.
3. Grey squares = no tile for that spot/zoom on the card. Check you're inside the
   area and zoom range you downloaded.

## Related

- [Maps](../features/Maps.md) — the map view feature and the Docker render
  pipeline alternative.
- [SD card layout](../reference/SD-Card-Layout.md) — the `/sd/meshcore` side of
  SD usage.
