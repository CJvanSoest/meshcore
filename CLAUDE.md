# CLAUDE.md

Guidance for Claude Code (and any contributor) working in this repository.

## What this is

A [MeshCore](https://meshcore.co.uk) LoRa mesh chat client for the **Tanmatsu
badge** (ESP32-P4 app processor, ESP32-C6 radio co-processor, SX1262). Single
ESP-IDF v5.5.1 image. One process; concurrency is a handful of FreeRTOS tasks
plus the `app_main` event loop. C11.

## Build, flash, test

```sh
make build  DEVICE=tanmatsu      # idf.py build → build/tanmatsu/application.bin
make upload DEVICE=tanmatsu      # badgelink AppFS upload (keeps the launcher)

cd tests && make test            # host gcc tests, run in CI before any IDF build
```

The Tanmatsu IDF toolchain must be set up first (see
[docs/wiki/Build-Deploy.md](docs/wiki/Build-Deploy.md)). Other board targets
live in `sdkconfigs/` (mch2022, hackerhotel-2024, heltecv3, kami, konsool,
esp32-p4-function-ev-board).

## Where things live

- `components/` — first-party source, split into `mc_*` components (see
  [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)); `main/` is just `main.c`. The
  module map and FreeRTOS task list are in [docs/wiki/Architecture.md](docs/wiki/Architecture.md).
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — **the authoritative discipline doc**:
  the six layers (L0 foundation … L5 app entry), the forbidden-include rules
  (grep-checkable, also enforced by `tests/check-arch-rules.sh`), and the
  wire-boundary rules. Read it before moving code between files.
- `tests/` — host-side gcc tests linked against the shipping `.c` files.
- `docs/wiki/` — long-form docs (protocol, UI, NVS, GPS, sounds, SD layout, C6 radio).

## Rules that matter here

1. **Do not modify vendored code.** `lodepng.{c,h}`, `qrcodegen.{c,h}`,
   `ed25519*.{c,h}` are third-party drops; `emoji_bitmaps.c` is generated asset
   data. Most TODO markers in the tree are upstream LodePNG comments, not work
   items. Leave them.
2. **`meshcore/` is the upstream protocol mirror.** Keep it pure (no UI, no
   BSP, no L1 headers) and do not extend a wire-format struct locally. A fix
   goes upstream first, then the dependency is re-pinned. Local additions on
   top of the protocol live in `radio.c`, never in `meshcore/`. When the
   compiler flags a warning in `meshcore/`, suppress it at the call site (CI
   target) rather than editing the mirror, as the test Makefile already does.
3. **Layer direction is one-way.** Higher layers include lower; never the
   reverse. `render_*.c` must not include `meshcore/`. See docs/ARCHITECTURE.md for
   the exact grep checks.
4. **Add a host test for any pure logic you touch.** Pure modules with no
   ESP-IDF / pax / mbedtls dependency (region_limits, meshcore codecs,
   gps_parser, the companion command parser) are unit-tested on the host and
   gate the merge. Keep that property; do not pull a platform header into a
   currently-pure translation unit without good reason.

## Conventions

- Every source file starts with an SPDX header
  (`SPDX-FileCopyrightText` + `SPDX-License-Identifier: MIT`).
- Formatting is `.clang-format` (run `clang-format -i` on touched files).
- Comments are sparse: explain the non-obvious (a wire quirk, a locking
  coupling), not the obvious.
- Commit messages and all repo text are in English.

## Gotchas worth knowing

- **Time comes from the C6 RTC, not SNTP.** `app_main` brings up the P4↔C6
  stack but does not connect WiFi or run SNTP; it reads the clock via
  `bsp_rtc_update_time`. `identity_sntp_sync_cb` is a leftover from the old
  SNTP path and is never registered.
- **The contacts table is protected by `node_mutex`**, not a contacts-specific
  mutex (`send_advert_direct` walks `contacts[]` under `node_mutex`). Hold the
  right lock when touching either table.
- **Two radio-firmware version strings track the C6 independently**
  (`app_config.h` `TANMATSU_RADIO_FW_LABEL` and
  `radio_system_protocol_client.h`). Bump both together on a C6 reflash.
