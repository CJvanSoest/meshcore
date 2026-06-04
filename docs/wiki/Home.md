# MeshCore for Tanmatsu — Wiki

Reference documentation for developers, contributors and curious users.

For end-user setup see the [README](../../README.md).

## What's new in v2.2.0 (2026-06-04)

- **Tile-grid home screen** in LilyGo-Pager visual style replaces the
  boot-to-Settings landing. 8 tiles: Nodes / DM / Channel / Map (soon) /
  Advert / Settings / About / QR. ESC walks back to home, ESC on home
  returns to the launcher.
- **Shared Pager status strip** on every classic view: view name + inline
  DM / # unread badges on the left, RX / TX (rolling 1 h duty cycle %) /
  battery on the right. Replaces the old four-column tab bar.
- **Settings is now a two-level menu** — tile-grid of 6 categories
  (Identity, Regulatory, Radio, Network, Region & Location, Brightness)
  → drill into one category to see only its fields.
- **Brightness category** (per-app display / keyboard / LED sliders,
  NVS-persisted; backlog #47).
- **About tile** → VIEW_ABOUT with version + build date + author +
  upstream credits + MIT license + source URL.
- **Single-flush render dispatcher** kills the QR + emoji overlay
  flicker; QR overlay now shows `[ESC] [X] [Enter] to close` in amber.
- **`tanmatsu-lora` switched to upstream `^0.3.0`** now that rx_boost
  landed there; radio firmware label bumped to `v3.2.0`. The CJ Gitea
  forks of `tanmatsu-radio` + `tanmatsu-lora` are retired (zero delta).

## Pages

- **[Architecture](Architecture.md)** — modules and how they interact
- **[MeshCore protocol](MeshCore-Protocol.md)** — packet types, encryption, ADVERT/DM/Channel/PATH formats
- **[UI / UX](UI-UX.md)** — tabs, input modes, QR overlay, edit-mode state machine, Settings section headings & hints
- **[Settings / NVS](Settings-NVS.md)** — every persistent key, default, range, and the regulatory/country compliance tables
- **[SD card layout](SD-Card-Layout.md)** — `/sd/meshcore/` structure, encryption, self-heal
- **[C6 radio communication](C6-Radio.md)** — `lora_rpc`, RSSI/SNR patches, supported frequencies
- **[Firmware versions](Firmware-Versions.md)** — our radio/lora/launcher forks vs. Nicolai upstream, and what has converged
- **[Build / Deploy](Build-Deploy.md)** — IDF env, badgelink upload, launcher dependency, partition layout
