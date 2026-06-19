# MeshCore for Tanmatsu — Wiki

Reference documentation for developers, contributors and curious users.

For end-user setup see the [README](../../README.md).

## What's new since v2.2.0

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
  header or `?key=` query param. See [GPS sources](GPS-Sources.md).
- **PA1010D GPS auto-fill** — one-press scan from
  **Settings → Region & Location → Auto-fill from GPS**, reads NMEA over
  the QWIIC bus, commits `lat_e6` / `lon_e6` to NVS with source tag
  `GPS_SRC_PA1010D`.
- **BLE companion radio** (toggle in Settings) — pair with the iPhone
  MeshCore app to use the badge as its radio. Pair UI + SMP passkey
  toast wired; lat/lon write path is preview (see
  [GPS sources](GPS-Sources.md)).
- **Refactor hygiene** — handle_nav / handle_key split per view, field
  registry replaces parallel save + builder tables, lora_rx_task split
  per packet type. Net: same code size, much less duplication, adding
  a settings field or packet type is now a localised edit.

## Pages

- **[Architecture](Architecture.md)** — modules and how they interact
- **[MeshCore protocol](MeshCore-Protocol.md)** — packet types, encryption, ADVERT/DM/Channel/PATH formats
- **[UI / UX](UI-UX.md)** — tabs, input modes, QR overlay, edit-mode state machine, Settings section headings & hints
- **[Settings / NVS](Settings-NVS.md)** — every persistent key, default, range, and the regulatory/country compliance tables
- **[Maps](Maps.md)** — VIEW_MAP feature: tile cache, profile system, zoom advice, **server-side rendering pipeline** + hardware requirements
- **[GPS sources](GPS-Sources.md)** — Manual / PA1010D / HTTPS `/ping` / USB-CDC / BLE, including OwnTracks + iOS Shortcuts setup and what's tested vs preview
- **[Sounds](Sounds.md)** — WAV format, recommended free sources, `ffmpeg` + `badgelink` setup
- **[SD card layout](SD-Card-Layout.md)** — `/sd/meshcore/` structure, encryption, self-heal
- **[C6 radio communication](C6-Radio.md)** — `lora_rpc`, RSSI/SNR patches, supported frequencies
- **[Firmware versions](Firmware-Versions.md)** — our radio/lora/launcher forks vs. Nicolai upstream, and what has converged
- **[Build / Deploy](Build-Deploy.md)** — IDF env, badgelink upload, launcher dependency, partition layout
