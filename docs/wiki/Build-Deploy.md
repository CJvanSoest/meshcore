# Build / Deploy

## Toolchain

| | |
|---|---|
| Framework | **ESP-IDF v5.5.1** |
| Target | `esp32p4` (Tanmatsu app processor) |
| Tools path | `.IDF_TOOLS_PATH` file in repo root |
| IDF path | `.IDF_PATH` file in repo root |

Both files are populated by cloning Nicolai Electronics'
[tanmatsu-template-pax](https://github.com/Nicolai-Electronics/tanmatsu-template-pax)
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

MeshCore requires a **patched local checkout** of the
[tanmatsu-launcher](https://github.com/Nicolai-Electronics/tanmatsu-launcher)
with the following two patches applied on top of the upstream `main` branch:

| Patch | What it does | Why |
|---|---|---|
| WiFi auto-connect on boot (`main/main.c` around the wifi-init block) | Always call `wifi_connect_try_all()` regardless of NTP state | Stock launcher only reconnects when NTP is enabled — MeshCore needs WiFi for SNTP and to keep the WiFi-LED meaningful |
| badge-elf-api / badge-bsp type fix | Renames `lcd_color_rgb_pixel_format_t` → `bsp_display_color_format_t` (and friends) in the managed_components copy | Upstream `badge-elf-api 0.6.0` was not bumped for `badge-bsp 0.9.9` signature change; build fails otherwise with `-Werror=incompatible-pointer-types` |

After an upstream launcher release, check whether these have been folded in;
if not, re-apply.

## Required launcher version

`v0.1.2` (`badge-launcher 0.1.2`) or newer — earlier versions show LoRa
frequencies in MHz wrongly (Hz fix, PR #91).

## tanmatsu-radio (C6) firmware

Built separately from the
[`tanmatsu-radio`](https://github.com/CJvanSoest/tanmatsu-radio) checkout
(or upstream + local patches). Use the C6 sub-toolchain (esp32c6 target,
same IDF version). Flash via esptool over USB after pressing `U` in the
MeshCore Settings tab. Full instructions in
[C6 Radio](C6-Radio#firmware-update-workflow).

## Build env quirks

| Symptom | Cause | Fix |
|---|---|---|
| Build cached for wrong target (`esp32` instead of `esp32p4`) | Stale `build/tanmatsu/` from a different target | `rm -rf build/tanmatsu` and rebuild |
| `idf.py` warns about `python_env` mismatch | Wrong `IDF_TOOLS_PATH` exported | Re-export from the repo's `.IDF_PATH` and `.IDF_TOOLS_PATH` files (or whichever ESP-IDF install the tools were pinned to) |
| `make upload` fails with "Device not configured" | Serial port unable to open | Make sure no monitor is attached; for raw read with DTR/RTS off, set `s.dtr = False; s.rts = False` before opening |
| badge-elf-api type mismatch after `rm dependencies.lock + reconfigure` | The local patches in `managed_components/badgeteam__badge-elf-api/` were wiped | Re-apply the patches documented in [Launcher dependency](#launcher-dependency) |
