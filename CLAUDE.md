# CLAUDE.md

Guidance for Claude Code (and any contributor) working in this repository.

## What this is

A [MeshCore](https://meshcore.co.uk) LoRa mesh chat client for the **Tanmatsu
badge** (ESP32-P4 app processor, ESP32-C6 radio co-processor, SX1262). Single
ESP-IDF v5.5.1 image. One process; concurrency is a handful of FreeRTOS tasks
plus the `app_main` event loop. C11.

## Start here

Before changing code, read the contributor handbook in [`.claude/`](.claude).
It is the same model as the docs, written as working rules for an AI pair
programmer or any contributor:

- [`.claude/Guidelines.md`](.claude/Guidelines.md) — the index: mental model,
  where code goes, hard rules, the green gate.
- [`.claude/Components.md`](.claude/Components.md) — per-component map +
  dependency graph + constants.
- [`.claude/Data-Flows.md`](.claude/Data-Flows.md) — cold start, RX / TX /
  advert / DM / channel flows with real function names.
- [`.claude/Crypto.md`](.claude/Crypto.md) — signing, channel, DM, region scope,
  the ed25519 split, the gates. Read before touching crypto.
- [`.claude/Testing.md`](.claude/Testing.md) — host harness, what each gate
  proves, golden vectors, adding a test.
- [`.claude/Build-And-CI.md`](.claude/Build-And-CI.md) — build + CI.
- [`.claude/Workflow.md`](.claude/Workflow.md) — first read to green commit.
- [`.claude/Pitfalls.md`](.claude/Pitfalls.md) — traps that already cost time
  or shipped broken behaviour. Read before trusting a tool or assumption.

The design rationale and the "how to program here" model are in
[docs/architecture/Blueprint.md](docs/architecture/Blueprint.md); the enforceable rules in
[docs/architecture/Architecture.md](docs/architecture/Architecture.md).

## Build, flash, test

```sh
make build  DEVICE=tanmatsu      # idf.py build → build/tanmatsu/application.bin
make upload DEVICE=tanmatsu      # badgelink AppFS upload (keeps the launcher)

cd tests && make test            # host gcc tests, run in CI before any IDF build
```

The Tanmatsu IDF toolchain must be set up first (see
[docs/guides/Build-Deploy.md](docs/guides/Build-Deploy.md)). Other board targets
live in `sdkconfigs/` (mch2022, hackerhotel-2024, heltecv3, kami, konsool,
esp32-p4-function-ev-board).

## Where things live

- `components/` — first-party source, split into `mc_*` components (see
  [docs/architecture/Architecture.md](docs/architecture/Architecture.md)); `main/` is just `main.c`. The
  module map and FreeRTOS task list are in [docs/architecture/Overview.md](docs/architecture/Overview.md).
- [docs/architecture/Architecture.md](docs/architecture/Architecture.md) — **the authoritative discipline doc**:
  the six layers (L0 foundation … L5 app entry), the forbidden-include rules
  (grep-checkable, also enforced by `tests/lint/check-arch-rules.sh`), and the
  wire-boundary rules. Read it before moving code between files.
- `tests/` — host-side gcc tests linked against the shipping `.c` files.
- `docs/` — long-form docs (protocol, UI, NVS, GPS, sounds, SD layout, C6 radio).

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
   reverse. `render_*.c` must not include `meshcore/`. See docs/architecture/Architecture.md for
   the exact grep checks.
4. **Add a host test for any pure logic you touch.** Pure modules with no
   ESP-IDF / pax / mbedtls dependency (region_limits, meshcore codecs,
   gps_parser, the companion command parser) are unit-tested on the host and
   gate the merge. Keep that property; do not pull a platform header into a
   currently-pure translation unit without good reason.

## Conventions

- Every source file starts with an SPDX header
  (`SPDX-FileCopyrightText` + `SPDX-License-Identifier: MIT`).
- Formatting is `.clang-format`, gated in CI by `check-format.sh`. Run
  `clang-format -i` (canonical version **18.1.8**) on touched files; vendored
  code and the `meshcore/` mirror are excluded.
- Comments are sparse: explain the non-obvious (a wire quirk, a locking
  coupling), not the obvious.
- Commit messages and all repo text are in English.

## Gotchas worth knowing

- **Time comes from the C6 RTC, not SNTP.** `app_main` brings up the P4↔C6
  stack but does not connect WiFi or run SNTP; it reads the clock via
  `bsp_rtc_update_time`. There is no in-app SNTP path.
- **The contacts table is protected by `node_mutex`**, not a contacts-specific
  mutex (`send_advert_direct` walks `contacts[]` under `node_mutex`). Hold the
  right lock when touching either table.
- **Two radio-firmware version strings track the C6 independently**
  (`app_config.h` `TANMATSU_RADIO_FW_LABEL` and
  `radio_system_protocol_client.h`). Bump both together on a C6 reflash.
