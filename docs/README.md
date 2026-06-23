# MeshCore for Tanmatsu — Documentation

Reference documentation for developers, contributors and curious users.

For end-user setup see the [README](../README.md).

## What's new since v2.2.0

- **Toolbox (v2.7.0)** — a Settings tile launching LoRa diagnostic tools.
  **Packet Log**: a live hex / dissector view of every RX and TX frame, with
  pause, scroll, a per-frame detail view, and an `E` export of the capture ring
  to `/sd/meshcore/log/pkt_<unix>.csv`. **Coverage Test**: ping discovered
  repeaters 3x with an upstream MeshCore TRACE, colour-code reachability, and
  log every GPS-stamped attempt (+ uplink/downlink SNR) to one CSV per session.
  See [Toolbox](features/Toolbox.md) and
  [UI / UX](features/UI-UX.md).
- **Carto-only map styles (v2.7.0)** — the Settings "Style" picker now cycles
  only the styles in a one-line `MAP_PROFILES_ENABLED[]` list (Carto by default,
  matching the tileset shipped on SD); one line re-enables CyclOSM / OpenTopo /
  Ripple. See [Maps](features/Maps.md).
- **Screensaver + robustness (v2.6.0)** — the screensaver also blanks the
  keyboard backlight and wakes on any key; plus a batch of receive-path fixes
  (1-byte sender-hash collisions, a duty-cycle accounting race, node-table
  eviction). See the [changelog](CHANGELOG.md).
- **Notification sounds** — drop your own WAVs on
  `/sd/meshcore/sounds/{1..4}.wav`, pick one per event (DM / Channel /
  Error / Boot) in **Settings → Sounds**. Volume + on/off per event,
  all NVS-persisted.
- **On-device HTTPS cert** — each badge generates its own ECDSA P-256
  self-signed cert on first boot, persisted to NVS. Replaces the
  embedded shared cert + 70-line static SAN list.
- **mDNS publishing** — the badge announces `tanmatsu.local` + the
  `_https._tcp:8443` service so iOS / curl / OwnTracks resolve it by
  name on the LAN without a hardcoded IP.
- **HTTPS `/ping` endpoint** — accepts MeshMapper batches, iOS Shortcut
  flat pushes, and OwnTracks HTTP-mode bodies. Auth via `X-API-Key`
  header or `?key=` query param. See [GPS sources](features/GPS-Sources.md).
- **PA1010D GPS auto-fill** — one-press scan from
  **Settings → Region & Location → Auto-fill from GPS**, reads NMEA over
  the QWIIC bus, commits `lat_e6` / `lon_e6` to NVS with source tag
  `GPS_SRC_PA1010D`.
- **BLE companion radio** (toggle in Settings) — pair with the iPhone
  MeshCore app to use the badge as its radio. Pair UI + SMP passkey
  toast wired; lat/lon write path is preview (see
  [GPS sources](features/GPS-Sources.md)).
- **Refactor hygiene** — handle_nav / handle_key split per view, field
  registry replaces parallel save + builder tables, lora_rx_task split
  per packet type. Net: same code size, much less duplication, adding
  a settings field or packet type is now a localised edit.

## Pages

The docs are grouped into four folders.

### `architecture/` — how the app is built
- **[Blueprint](architecture/Blueprint.md)** — design rationale and how to program in this project (read first)
- **[Architecture (rules)](architecture/Architecture.md)** — the enforced layers, forbidden includes, wire boundary
- **[Module overview](architecture/Overview.md)** — the modules and how they interact
- **[Components](architecture/Components.md)** — the component map, dependency graph, constants

### `guides/` — building and contributing
- **[Build / Deploy](guides/Build-Deploy.md)** — IDF env, badgelink upload, launcher dependency, partition layout
- **[Contributing](guides/CONTRIBUTING.md)** — the contributor checklist and code rules
- **[Releases](guides/Releases.md)** — the fixed release format and how to cut a release

### `reference/` — protocol and hardware reference
- **[MeshCore protocol](reference/MeshCore-Protocol.md)** — packet types, encryption, ADVERT/DM/Channel/PATH formats
- **[Settings / NVS](reference/Settings-NVS.md)** — every persistent key, default, range, the regulatory tables
- **[C6 radio](reference/C6-Radio.md)** — `lora_rpc`, RSSI/SNR patches, supported frequencies
- **[Firmware versions](reference/Firmware-Versions.md)** — the radio/lora/launcher forks vs upstream
- **[SD card layout](reference/SD-Card-Layout.md)** — `/sd/meshcore/` structure, encryption, self heal

### `features/` — what the app does
- **[UI / UX](features/UI-UX.md)** — views, key bindings, the edit-mode state machine, the QR overlay, the Toolbox
- **[Maps](features/Maps.md)** — the slippy map: tile cache, the Carto-only style picker, the rendering pipeline
- **[Toolbox](features/Toolbox.md)** — the Packet Log and Coverage Test (TRACE-based reachability + the wire gotchas)
- **[GPS sources](features/GPS-Sources.md)** — the five input paths and how to wire OwnTracks / iOS Shortcuts / MeshMapper
- **[Sounds](features/Sounds.md)** — WAV format, recommended free sources, the upload recipe
- **[Screenshots](features/Screenshots.md)** — every view, full set

### Release history
- **[Changelog](CHANGELOG.md)** — what changed per version
