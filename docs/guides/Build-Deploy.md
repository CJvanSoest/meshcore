# Build / Deploy

## Toolchain

| | |
|---|---|
| Framework | **ESP-IDF v6.0.2** |
| Target | `esp32p4` (Tanmatsu app processor), chip revision 1 and up |
| BSP | `badge-bsp` 2.x (needed for boards with the second display panel) |
| Tools path | `.IDF_TOOLS_PATH` file in repo root |
| IDF path | `.IDF_PATH` file in repo root |

Both files are populated by cloning Nicolai Electronics'
[tanmatsu-template](https://github.com/Nicolai-Electronics/tanmatsu-template)
once and pointing this repo at the same IDF checkout.

## One-shot build

The repo ships a `Makefile` that captures the flag soup:

```sh
make build  DEVICE=tanmatsu        # produces build/tanmatsu/*
make upload DEVICE=tanmatsu        # badgelink appfs upload
make clean
```

Behind the scenes:

```sh
idf.py -B build/tanmatsu build \
  -DDEVICE=tanmatsu \
  -DSDKCONFIG_DEFAULTS="sdkconfigs/general;sdkconfigs/tanmatsu" \
  -DSDKCONFIG=sdkconfig_tanmatsu \
  -DIDF_TARGET=esp32p4 \
  -DFAT=0
```

## Upload paths

| Method | When | Side-effects |
|---|---|---|
| `make upload` (badgelink) | Normal dev loop | **Preserves launcher**, writes to `appfs` partition. Launcher tile shows the generic `ICON_APP` (no custom icon for AppFS-only apps). |
| SD-card bundle (see below) | Release / appstore artifact | Custom tile icon, full metadata.json, executable in `/sd/apps/<slug>/`. |
| `idf.py flash` | First-time provisioning | **Overwrites launcher**, full chip flash |
| `idf.py monitor` | Reading logs | Read-only |

For day-to-day work always use `make upload`. After the upload the badge
auto-launches the new MeshCore build because the launcher's last-run
preference points to it.

### SD-card bundle (custom icon, appstore-ready)

The launcher's `app_metadata_parser` reads `<slug>/metadata.json` from
`/sd/apps/` and loads the icon referenced under `icon.32x32`. Bundle
contents shipped from `assets/`:

| File | Purpose |
|---|---|
| `metadata.json` | name, version, author, license, executable map, `icon.32x32` |
| `icon-32.png` (uploaded as `icon32.png`) | 32×32 RGBA, launcher tile graphic |
| `icon-256.png` | hi-res asset for store listings / marketing |
| `generate_icon.py` | reproducible Python source for both PNGs |

Upload procedure:

```sh
BL=path/to/badgelink.sh
$BL fs mkdir   /sd/apps/nl.cj.meshcore
$BL fs upload  /sd/apps/nl.cj.meshcore/metadata.json  assets/metadata.json
$BL fs upload  /sd/apps/nl.cj.meshcore/icon32.png     assets/icon-32.png
$BL fs upload  /sd/apps/nl.cj.meshcore/meshcore.bin   build/tanmatsu/application.bin
```

After upload reboot the launcher (ESC from MeshCore) — the tile picks up
the new icon. Same bundle layout is what an appstore client would
distribute.

## Partition layout (16M.csv)

| Name | Offset | Size | Purpose |
|---|---|---|---|
| `nvs` | 0x9000 | 0x10000 | Persistent settings |
| `otadata` | 0x1D000 | 0x2000 | Currently unused (no OTA) |
| `ota_0` | 0x20000 | 0x400000 | Launcher app |
| `appfs` | 0x420000 | 0x800000 | MeshCore + other apps (badgelink target) |
| `locfd` | 0xC20000 | rest | Reserved / future |

## Launcher dependency

MeshCore runs on the stock [tanmatsu-launcher](https://github.com/Nicolai-Electronics/tanmatsu-launcher).
No local launcher patches are needed any more: WiFi auto connect on boot is
upstream (PR #141) and the old badge-bsp type fix was dropped when the
launcher moved to a newer BSP.

## Required launcher version

Launcher `v0.3.1` with `tanmatsu-radio` `v3.4.0` is what v3.8.0 was tested
on. Older launchers show LoRa frequencies in MHz wrongly (Hz fix, PR #91,
`v0.1.2`), and this build targets BSP 2 / ESP-IDF 6, so keep the launcher
current via **Tools → Firmware update**.

## Chip revision (ESP-IDF 6)

ESP-IDF 6 builds for the ESP32-P4X (chip revision 3) by default, and the
bootloader on the badge drops such an image: the app shows a blue screen and
prints nothing. `sdkconfigs/tanmatsu` therefore pins the chip revision with
`CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`, `CONFIG_ESP32P4_REV_MIN_1=y`,
`CONFIG_ESP32P4_REV_MIN_FULL=1` and `CONFIG_ESP_REV_MIN_FULL=1`. Keep these
lines when you change the sdkconfig defaults.

## Building in Docker

No local toolchain needed:

```sh
docker run --rm -v "$PWD":/project -w /project espressif/idf:v6.0.2 \
  bash -lc 'git config --global --add safe.directory /project && \
  idf.py -B build/tanmatsu build -DDEVICE=tanmatsu \
  -DSDKCONFIG_DEFAULTS="sdkconfigs/general;sdkconfigs/tanmatsu" \
  -DSDKCONFIG=sdkconfig_tanmatsu -DIDF_TARGET=esp32p4 -DFAT=0'
```

The `safe.directory` line matters: without it `git describe` fails silently and
the app reports version `0.0.0`. On a shared drive that does not keep file
modes, also run `git config core.fileMode false`, or the version gets a
`-dirty` suffix.

## tanmatsu-radio (C6) firmware

Built separately from the
[`tanmatsu-radio`](https://github.com/CJvanSoest/tanmatsu-radio) checkout
(or upstream + local patches). Use the C6 sub-toolchain (esp32c6 target,
same IDF version). Flash via esptool over USB with the C6 in DFU mode
(power-cycle into bootloader via BSP). End users update the C6 through
**Launcher → Tools → Firmware update** instead. Full instructions in
[C6 Radio](../reference/C6-Radio.md#firmware-update-workflow).

## Build env quirks

| Symptom | Cause | Fix |
|---|---|---|
| Build cached for wrong target (`esp32` instead of `esp32p4`) | Stale `build/tanmatsu/` from a different target | `rm -rf build/tanmatsu` and rebuild |
| `idf.py` warns about `python_env` mismatch | Wrong `IDF_TOOLS_PATH` exported | Re-export from the repo's `.IDF_PATH` and `.IDF_TOOLS_PATH` files (or whichever ESP-IDF install the tools were pinned to) |
| `make upload` fails with "Device not configured" | Serial port unable to open | Make sure no monitor is attached; for raw read with DTR/RTS off, set `s.dtr = False; s.rts = False` before opening |
