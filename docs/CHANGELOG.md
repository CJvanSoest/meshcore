# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/). The exact
release format, entry style and the steps for cutting a release are fixed in
[Releases.md](guides/Releases.md); follow it rather than inventing per release wording.

Sections per release:
- **Added** — new features
- **Changed** — changes in existing functionality
- **Deprecated** — soon-to-be removed features
- **Removed** — removed features
- **Fixed** — bug fixes
- **Security** — vulnerabilities and mitigations

If a section for a release is missing from this file when running
`scripts/release.sh`, the release notes fall back to a categorized list
of merged PR titles since the previous tag.

## [Unreleased]

## [2.8.0] - 2026-06-30

### Added
- **Private channels** — create a channel with a random key and share it by QR
  or secret, or join one by entering a name plus its 32 hex secret. Up to 15
  channels (PR #34).
- **Emoji** — the inline emoji set grows from 8 to 40, with a paged picker (PR #42).
- **HOME Exit tile** — a tile on the 3x3 home grid returns to the launcher,
  alongside the existing red-X / ESC shortcut (PR #38).
- **Packet Log columns** — the live log gains column headers plus SNR and hop
  count as their own columns (PR #37).
- **Coverage Test, nearby repeaters** — the list now holds up to 64 repeaters
  and filters to those within 15 km of your position, so nearby repeaters are no
  longer hidden behind the old 32 cap (PR #45).
- **Channel message status** — your own channel messages show a relayed / not
  sent indicator (PR #33).

### Changed
- **UI font** — the proportional face is now Montserrat for a rounder, more
  legible look; footer hints, column headers and tile spacing were tuned to
  match (PR #40).
- **Coverage Test, direct only** — a repeater counts as reachable only when the
  ping returns within one hop (direct or a single relay), so the result reflects
  direct coverage rather than multi-hop reachability (PR #45).
- **Back navigation** — the red X is the sole back key; ESC exits only from the
  home screen (PR #24, PR #26).

### Fixed
- **Companion frequency** — the phone app showed e.g. 869618.000 MHz because the
  self-info frequency was sent in Hz; it now reports kHz, so the app shows
  869.618 MHz (PR #44).
- **Duplicate channel messages** — your own channel messages no longer appear
  twice; the self-flood echo is dropped on receive (PR #32).

### Removed
- **GPS auto-fill** — the "Auto-fill from GPS" settings action is gone.
  Coordinates are set manually or via the MeshCore phone app, and the on-device
  GPS module feeds only the live map (PR #44).

## [2.7.0] - 2026-06-23

### Added
- **Toolbox** — a new tile under Settings opening a launcher for LoRa
  diagnostic tools. First tool: **Packet Log**, a live hex-dump / dissector
  view of received and transmitted radio frames, with pause, scroll and clear
  (#3).
- **Toolbox → Coverage Test** — probe discovered repeaters 3x with an upstream
  MeshCore TRACE and colour-code reachability (green/orange/red), logging every
  GPS-stamped attempt plus uplink/downlink SNR to one CSV per session on the SD
  card (#3).
- **Toolbox → Packet Log → export to SD** — the `E` key dumps the capture ring
  to a plaintext CSV `/sd/meshcore/log/pkt_<unix>.csv`
  (`ts_ms,dir,type,route,rssi_dbm,snr_db,len,raw_hex`), newest frame first, for
  off-badge analysis. Row formatting is the host-tested pure `diag_csv_row()`.

### Changed
- **Map styles default to Carto-only.** The Settings "Style" picker now cycles
  only the styles listed in `MAP_PROFILES_ENABLED[]` (Carto by default), and the
  NVS loader clamps a stored-but-disabled style to that default. Enable CyclOSM /
  OpenTopoMap / Ripple by editing the one-line list (see Maps.md "Enabled
  styles"); no other code change needed.

## [2.6.0] - 2026-06-19

### Changed
- **The screensaver also blanks the keyboard backlight**, and now wakes on any
  key instead of only F3. A key press while blanked restores the screen and
  keyboard backlight and is itself ignored (it triggers no function); the RGB
  notification LED keeps blinking throughout, so in-pocket alerts still show.
  (issue #7)

### Fixed
- **DM and ACK decrypt under a 1-byte sender-hash collision.** A TXT_MSG
  carries only a 1-byte sender hash; receive resolved it to the first
  node whose pubkey started with that byte, so when two nodes collided the
  wrong key ran the ECDH and decrypt failed (send was unaffected because it
  uses the full contact pubkey). `rx_handle_dm` and `rx_handle_path` now try
  every candidate whose pubkey byte matches and let the MAC/ACK decide, the
  same way channel RX brute-forces keys. Collisions are ~1/256 per node pair.
- **Duty-cycle accounting data race.** The rolling-hour airtime buckets and
  their cached sum were read-modified-written by several TX tasks (advert,
  direct-advert, the RX-task ACK, UI sends) with no lock, despite a comment
  claiming `rx_mutex` was held. A corrupted sum could let TX slip past the
  regulatory budget. Now guarded by a dedicated `dc_mutex`.
- **Node table eviction reused a slot as the wrong identity.** When the table
  was full, the evicted slot kept the previous node's name, packet count,
  position and signal stats while taking the new node's pubkey. A slot is now
  treated as new whenever its pubkey differs from the advert, and reset.
- **Node LRU eviction picked the wrong slot** once any pre-sync
  (`last_seen_unix == 0`) node was scanned, because it poisoned the unix
  comparison. The two clocks are now ranked in separate trackers.
- **Contacts table touched without `node_mutex` on the RX task.** The sender
  resolver read `contacts[]` unlocked, and `rx_handle_dm` wrote it via
  `contact_ensure` / `contact_mark_unread` unlocked, racing UI-thread edits
  that shift the array. All three now hold `node_mutex`.
- **Settings keyboard navigation opened the wrong category.** The keyboard
  Enter path used the visible-slot cursor as the real category index and
  clamped with the real (not visible) count, diverging from the D-pad path;
  it now translates through `settings_visible_category_real_idx`.
- **System-protocol GET request shipped uninitialised stack bytes** to the C6
  (only the header was filled). The request buffer is now zeroed.

### Added
- `test_advert_sign` host test: locks the ADVERT signature layout (the
  `to_sign` byte range) with an offset check plus a golden signature, so a
  regression in the signed-bytes construction goes red in CI rather than
  only showing up as silently-rejected adverts on hardware.

### Changed
- Extracted the ADVERT signable-bytes construction out of
  `send_advert_internal` into the pure, host-tested
  `meshcore_advert_signable_bytes` (`mc_proto/advert_sign.c`). No wire
  change; both flood and direct adverts sign the same bytes as before.

### Removed
- Dead first-party functions with no caller in the tree or tests:
  `nodes_mark_dirty` / `nodes_dirty`, `save_wifi`, the unused
  `nvs_{load,save}_{u16,u32,i32}` scalar wrappers,
  `http_server_is_running`, `map_tile_to_latlon`, and the `icon_placeholder`
  home tile. See the "Unused code" section in `docs/architecture/Architecture.md`.

## [2.5.0] - 2026-06-18

### Added
- **VIEW_MAP** — slippy-map view that renders OSM PNG tiles from SD with
  live GPS overlay. Pan, zoom (6 → 14), crosshair, scale bar, lock toggle,
  attribution. Status strip shows zoom / SAT count / RX state / battery.
  Persists centre + zoom + lock to NVS so it comes back where you left it.
- **Map style profiles** — Ripple / Carto / Cycle / Topo. Settings →
  Region & Location → Map → Style selects which `/sd/maps/<profile>/`
  the tile reader consumes. Profile switch clears the LRU cache.
- **Live GPS background task** — polls a PA1010D over the QWIIC bus and
  drives the map crosshair. Four tracking profiles (Walking, Cycling,
  Driving, Manual) trade off update interval vs. battery.
- **Async tile-loader task** — render task never blocks on SD I/O.
  Producer/consumer pattern with a 128-slot queue and an LRU cache of
  36 slots in PSRAM. Zoom changes drain the queue so a new zoom level
  starts on a clean slate instead of waiting behind stale requests.
- **Off-screen prefetch ring** — the renderer requests the tile ring
  outside the visible viewport so pans land on warm tiles instead of
  grey placeholders.
- **Node pins with role-specific shapes** — chat=circle, repeater=square,
  room=diamond, sensor=triangle. Contacts/favorites get a white outline
  ring. Nearest-to-crosshair node name is labelled inline.
- **Chat UX polish (v2.5)** — message bubbles, unread indicators per
  conversation, network grouping in the nodes view, WAV filenames now
  show their source slot for easier debugging.
- **WiFi launcher-slot picker + on/off toggle** — drop the in-app
  SSID/password editor in favour of the launcher's polished WiFi-config
  UI. App-side responsibility: which of the stored slots to use, and
  whether WiFi is on right now.

### Changed
- **`MAP_ZOOM_MAX` bumped 10 → 14** — street-name scale is the new ceiling.
- **`FIELD_BLE_ENABLED` moved** from Region & Location → Network so it
  sits next to the other connectivity toggles.
- **Map legend moved top-right → top-left** — keeps the scale bar /
  attribution corner clear.

### Fixed
- **Direct-advert hang** — `send_advert_direct` used to iterate the full
  `node_list[]` (up to 200 background-discovered peers) while holding
  `node_mutex`, which froze the UI for ~30 s and ate ~3 % duty cycle per
  press. Now iterates `contacts[]` (max 16, ~1 % TX) from a background
  task so the UI stays responsive.
- **z=13 stays grey after zoom-out/in** — rapid zoom sweeps used to fill
  the loader queue with stale requests for lower zoom levels, crowding
  out fresh requests for the new top zoom. Queue is drained on every
  zoom change and bumped 64 → 128 slots.

## [2.3.2] - 2026-06-06

### Fixed
- **Advert reception** (PR #14, GitHub issue #1) — adverts broadcast by
  the Tanmatsu were rejected at protocol level by every upstream MeshCore
  verifier (Heltec, T-Deck+, T1000-E, Xiao SX1262, Waveshare USB). Root
  cause: the in-tree ref10-style Ed25519 implementation produced wrong
  points for RFC 8032 test vectors. Rewrote `ed25519_create_keypair` and
  `ed25519_sign` to delegate all field/group arithmetic to `mbedtls_mpi`
  (same battle-tested layer that powers the working X25519 path). Slower
  per call (~250 ms scalar mult) but RFC 8032 TV1 sign-roundtrip now PASSes
  on boot. Confirmed end-to-end against a Heltec node and externally with
  Python's `cryptography.Ed25519PublicKey.verify`.
  - **Side-effect**: `node_pub_key` derives differently for the same NVS
    seed (now canonical encoding). Existing QR contacts see this device
    as a new identity and must re-add via QR.
- **`path_hash_size` setting now actually applied to outgoing packets**.
  Was stored + displayed in Settings → multibyte but `msg.bytes_per_hop`
  stayed at the implicit default for every TX (advert, DM, channel msg,
  PATH_RETURN). Now mirrors upstream `Mesh::sendFlood()`.
- **`sync_word` setting persisted to NVS**. Was pushed to C6 in-session
  but lost on the next cold boot. New `lora.sync` NVS key.

### Added
- **RFC 8032 TV1 sign-roundtrip self-test** runs in `identity_init()` at
  boot and exposes `ed25519_tv1_keypair_ok` / `ed25519_tv1_sign_ok` as
  globals so future Ed25519 regressions surface immediately.

## [2.3.1] - 2026-06-05

### Added
- **Auto-screensaver timer** (PR #8) — extension of the F3 manual
  display-blank from v2.3.0. Configurable idle timeout in
  Settings → Brightness → "Auto-blank display": Off / 30s / 1min /
  5min / 10min / 30min. When idle for the chosen interval the badge
  blanks the backlight the same way a short F3 press does (LoRa RX,
  keyboard backlight and notification LEDs keep running); F3 wake
  re-arms the timer.
- Yellow-icon hint in the Settings → Brightness footer, mirroring
  the home-screen one so users configuring auto-blank also see the
  manual F3 shortcut.

### Fixed
- Repeated F3 blank/wake cycles occasionally failed on the second
  wake because the brightness-on-blank capture could end up as 0.
  Exit-blank now calls `apply_brightness()` to pull the current
  Settings value, dropping the stale local cache.

## [2.3.0] - 2026-06-05

### Added
- **F3 (yellow square) toggles display backlight** (PR #5) — short press
  blanks the MIPI backlight while keyboard input, LoRa RX and the
  notification LEDs keep running. Second press restores the previous
  brightness. Power button stays reserved for firmware-level power-off.
  Home + Brightness footers gained a yellow-icon hint advertising the
  shortcut.
- **Time via C6 RTC, no in-app SNTP** (PR #6) — boot pulls the time
  the launcher already synced into the coprocessor RTC via
  `bsp_rtc_update_time()`. Drops the WiFi-associate + `esp_sntp_init`
  dance so the WiFi PHY stays in `esp_wifi_stop()` for the whole
  MeshCore session. Visibly faster boot; WiFi reconnects automatically
  when the user returns to the launcher.
- CI workflow (`.github/workflows/ci.yml`) — builds the
  tanmatsu target on every push and PR, uploads `application.bin` as
  artifact (PR #1).
- PR template (`.github/PULL_REQUEST_TEMPLATE.md`) — five sections:
  What / Why / Test plan / Breaking changes / Screenshots (PR #2).
- `Architecture.md` — six-layer model, three forbidden-include
  rules, wire-boundary discipline for upstream-compat (PR #3).
- Upstream-tracking comment on `radio_system_protocol_client.h` —
  demonstrates the per-file boundary-tracking pattern.
- `scripts/release.sh` — single command for cutting a release
  + GitHub with changelog + binary attached.

### Fixed
- **Preamble length now persists in NVS** (PR #7) — was a UI-editable
  field that silently reset to 16 on every restart because it wasn't
  in the `load_lora_from_nvs` / `save_lora_to_nvs` schema. Default
  bumped to 8 to match the MeshCore protocol standard.

### Changed
- Branch protection rule on `main`: direct push disabled, CI
  status check required, block-on-outdated enabled. Devlog files
  exempted via `unprotected_file_patterns`.

## [2.2.0] - 2026-06-04

### Added
- `VIEW_HOME` tile-grid landing screen in Pager-stijl — eight tiles
  with PAX-drawn icons (Nodes, DM, Channel, Map, Advert, Settings,
  About, QR).
- Settings drilldown — six category tiles (Identity, Regulatory,
  Radio, Network, Region & Location, Brightness); each opens a focused
  field list.
- Per-app brightness controls — three sliders (display / keyboard /
  LED), cycling through 5/10/25/50/75/100%, persisted in NVS.
- `VIEW_ABOUT` — pulls version from `esp_app_get_description()`, plus
  authors, credits, license, source URL.
- Pager-style status header on every view — view name + inline
  DM/channel unread badges + RX/TX/battery.

### Changed
- Render dispatcher rewritten to a single-flush model — every
  `render_*()` writes the framebuffer without blitting; the dispatcher
  blits exactly once at the end. Kills the v2.1.x QR + emoji overlay
  flicker.
- `render.c` split per view: `render_settings.c`, `render_nodes.c`,
  `render_chat.c`, `render_channel.c`, `render_home.c`, `render_about.c`.
  Cross-file declarations in `render_internal.h`.
- `tanmatsu-lora` dependency switched from the CJ fork to upstream
  `nicolaielectronics/tanmatsu-lora ^0.3.0` (rx_boost landed upstream).
- Radio FW label bumped to `v3.2.0` (upstream `tanmatsu-radio`).

### Fixed
- QR overlay close hint clarified to `[ESC] [X] [Enter] to close`.
- Advert tile no longer dimmed as "soon" placeholder — it has an
  action (`HOME_ACTION_SEND_ADVERT`), so it's live.

[Unreleased]: https://github.com/CJvanSoest/meshcore/compare/v2.2.0...HEAD
[2.2.0]: https://github.com/CJvanSoest/meshcore/releases/tag/v2.2.0
