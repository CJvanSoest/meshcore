# MeshCore for Tanmatsu

A [MeshCore](https://meshcore.co.uk) LoRa mesh communication app for the
**[Tanmatsu](https://tanmatsu.cloud) badge**.

> **Compatible with the MeshCore iOS/Android app** — send and receive encrypted
> direct messages and public channel chat over LoRa, fully interoperable with
> other MeshCore nodes.

---

## Device

| | |
|---|---|
| **Hardware** | Tanmatsu badge (rev 5+) |
| **Application processor** | ESP32-P4 |
| **Radio co-processor** | ESP32-C6 |
| **Radio chip** | SX1262 (LoRa, 868 MHz EU band) |
| **Display** | 4" MIPI DSI ST7701, native 480×800 — rendered as **800×480 landscape** (rotation 270°) |
| **Framework** | ESP-IDF v5.5.1 |

---

## Views

Boot drops you on a **tile-grid home screen** in LilyGo-Pager visual style.
Each tile opens a view; ESC walks you back to home, ESC on home returns to
the launcher.

| Tile | Opens |
|---|---|
| **Nodes** | Live list of heard nodes — role, RSSI/SNR, distance, last seen; saved contacts starred |
| **DM** | Inbox + per-contact end-to-end encrypted conversations (carries an unread badge on the tile itself) |
| **Channel** | Public channel chat (AES-128-ECB), per-channel rings, unread badge on the tile |
| **Map** | Reserved — slots in once an on-device map view lands |
| **Advert** | Sends a flood advert inline + 2-second toast; stays on home |
| **Settings** | Two-level menu: tile-grid of categories (Identity / Regulatory / Radio / Network / Region & Location / **Brightness** / **Sounds**) → drill into the fields for one category. Advert config is reached via the Home → Advert tile. |
| **About** | App version, build date, author, upstream credits, MIT license, source URL |
| **QR** | Opens the "add me as contact" QR overlay rooted at home |

A persistent **Pager-style status strip** runs across every classic view —
view name + inline DM / # unread badges on the left, RX count / TX (rolling
1-hour duty cycle %) / battery on the right. Tab cycles the four classic
views (Settings → Nodes → DM → Channel) for keyboard power users.

## Highlights

- **Full MeshCore interoperability** with the iOS/Android app (DM send/receive
  with delivery acknowledgement, channel chat)
- **End-to-end encryption** — Ed25519 keypair generated on first boot; DMs
  encrypted with ECDH + AES-128-ECB; channel uses shared key
- **Persistent chat history** on microSD (AES-CBC encrypted, self-heals on
  identity change)
- **Multi-channel** — public channel + user-added `#channels`, picker UI
  with add/delete, brute-force MAC verify on RX, region scope visible in header
- **Region scope on the wire** — `ROUTE_TYPE_TRANSPORT_FLOOD` with
  HMAC-SHA256 transport codes per upstream MeshCore (mc-radar compatible)
- **Regulatory compliance helper** — pick your country for per-sub-band
  frequency / power (ERP/EIRP) limits, off-band & over-power warnings, and
  **hard duty-cycle enforcement** (TX blocked when the rolling 1-hour airtime
  budget is spent); 30+ countries across EU 868/433, US/AU/NZ 915, JP, KR, IN, RU
- **Per-message metadata** — local time, hop count, ACK state inline under
  each chat bubble
- **Unread badges** on the home tiles + the Pager status strip for missed
  DM / channel messages — visible no matter which view you're in
- **Per-app brightness** — independent display backlight, keyboard backlight
  and RGB LED brightness sliders (5/10/25/50/75/100 %, NVS-persisted) that
  override the launcher globals while MeshCore is running
- **GPS coordinates from multiple sources** — Manual entry, PA1010D module
  on the QWIIC bus (one-press auto-fill), USB-CDC push from a companion,
  BLE companion (iPhone MeshCore app), or HTTPS `/ping` (MeshMapper /
  iOS Shortcuts / OwnTracks). The active source is surfaced in Settings;
  full details in [GPS sources](docs/GPS-Sources.md).
- **Notification sounds** — drop your own WAVs on `/sd/meshcore/sounds/{1..4}.wav`,
  pick one per event (DM / Channel / Error / Boot) in Settings → Sounds.
  Volume + on/off per event, all NVS-persisted.
  See [Sounds](docs/Sounds.md) for the WAV format, recommended free
  catalogues, and the `ffmpeg` + `badgelink` upload recipe.
- **On-device HTTPS** — each badge generates its own ECDSA P-256
  self-signed cert on first boot, persisted to NVS. mDNS publishes
  `tanmatsu.local:8443`; iOS installs the cert once and `/ping` accepts
  GPS pushes from OwnTracks, iOS Shortcuts and MeshMapper without per-IP
  cert juggling.
- **BLE companion radio** (toggle in Settings) — pair with the official
  MeshCore iPhone app to use the badge as its radio while the iPhone
  drives the UI.
- **Twemoji-based emoji picker** — 8 base smileys, UTF-8 round-trip with
  other MeshCore clients
- **QR contact sharing** — show a QR that the mobile app can scan directly
- **Saved contacts** — favourites stay in the list even when out of range
- **Live RSSI / SNR / noise floor** per heard node and as a glance line on
  both the home screen footer and the Settings drilldown bottom row
- **Message LED + battery indicator** on every screen
- **Time from the C6 RTC** (synced by the launcher); last known time persisted to NVS

For the full feature list, packet protocol, encryption details and key bindings,
see the [docs](docs/README.md).

---

## Installing

On a freshly-recovered Tanmatsu (flashed via `recovery.tanmatsu.cloud`):

1. After recovery, **boot into the launcher** and open **Tools → Firmware
   update**. This pulls the latest launcher and C6 radio firmware over
   the air. Recovery currently serves an older radio firmware
   (`ESP-HOSTED 2.1.0`) whose `lora_protocol` ABI predates the one
   MeshCore expects — the app will show "LoRa radio not available"
   until you run this step.
2. Install **MeshCore** from the appstore.
3. Open MeshCore. The Settings tab shows the live radio firmware
   version; nodes start arriving on the Nodes tab once the C6 receives
   adverts.

See [C6 Radio → Firmware update workflow](docs/C6-Radio.md#firmware-update-workflow)
for the long-form explanation, including custom-build flashing for
developers.

---

## Regulatory compliance

LoRa runs in licence-exempt ISM bands whose rules differ per country —
permitted frequencies, maximum transmit power and (in the EU) a duty-cycle
ceiling. Set your **Country** in the Settings tab and the app helps you stay
within them:

- **Frequency & power — soft warnings.** The Frequency, TX power and Country
  rows turn **red** when the frequency falls outside every allowed sub-band, or
  when your *effective* radiated power (TX power + antenna gain, as ERP/EIRP)
  exceeds the sub-band limit. The footer spells out the active sub-band's limits.
- **Antenna gain** — a dedicated field (editable only once a country is set)
  feeds the ERP/EIRP calculation so the power check reflects your real antenna.
- **Duty cycle — hard enforced.** A rolling 1-hour airtime budget is tracked per
  sub-band (Semtech time-on-air formula). When it is spent, transmits are
  **blocked** until airtime frees up. The Duty cycle row shows
  `used% (used s / budget s)` and `BLOCKED` when capped.

> Guidance helper, **not legal advice** — you remain responsible for operating
> within your local regulations.

### EU 863–870 MHz sub-bands (ERP)

The Netherlands and 18 other EU/EU-aligned countries use the full ETSI
EN 300 220 sub-band plan:

| Band | Range (MHz) | Max power | Duty cycle |
|---|---|---|---|
| g | 863.0–865.0 | 14 dBm ERP | 0.1% |
| g1 | 865.0–868.0 | 14 dBm ERP | 1% |
| g1' | 868.0–868.6 | 14 dBm ERP | 1% |
| g2 | 868.7–869.2 | 14 dBm ERP | 0.1% |
| **g3** | **869.4–869.65** | **27 dBm ERP** | **10%** |
| g4 | 869.7–870.0 | 14 dBm ERP | 1% |

> The default **869.618 MHz** sits in **g3** — the high-power, 10%-duty
> "MeshCore" band. The SX1262's 22 dBm conducted ceiling stays comfortably
> under the 27 dBm ERP limit.

### Other regions

| Region | Countries | Range (MHz) | Max power | Duty cycle |
|---|---|---|---|---|
| EU 433 | 433 SRD | 433.05–434.79 | 10 dBm ERP | 10% |
| US 915 | US, CA, MX | 902–928 | 30 dBm EIRP | none |
| AU/NZ 915 | AU, NZ | 915–928 | 30 dBm EIRP | none |
| JP 920 | JP | 920.5–923.5 | 13 dBm EIRP | 10% + LBT |
| KR 920 | KR | 920.0–923.0 | 14 dBm EIRP | 10% |
| IN 865 | IN | 865.0–867.0 | 30 dBm EIRP | none |
| RU 864/869 | RU | 864–865 / 868.7–869.2 | 14 dBm ERP | 0.1% |

Full per-country table and the data schema live in
[Settings / NVS → Regulatory compliance](docs/Settings-NVS.md#regulatory-compliance).

---

## Screenshots

Mock-ups of every view in landscape proportions (800×480), stacked vertically
so each one renders at a usable width regardless of viewport.

**Home — tile grid**
<p><img src="assets/screenshots/screen-home.svg" width="480"></p>

**Settings — category tiles**
<p><img src="assets/screenshots/screen-settings-tiles.svg" width="480"></p>

**Settings → Radio (drill-in)**
<p><img src="assets/screenshots/screen-settings.svg" width="480"></p>

**Settings → Brightness (drill-in)**
<p><img src="assets/screenshots/screen-settings-brightness.svg" width="480"></p>

**Nodes**
<p><img src="assets/screenshots/screen-nodes.svg" width="480"></p>

**DM inbox**
<p><img src="assets/screenshots/screen-dm-inbox.svg" width="480"></p>

**DM conversation**
<p><img src="assets/screenshots/screen-dm.svg" width="480"></p>

**Channel**
<p><img src="assets/screenshots/screen-channel.svg" width="480"></p>

**QR contact card**
<p><img src="assets/screenshots/screen-qr.svg" width="480"></p>

**About**
<p><img src="assets/screenshots/screen-about.svg" width="480"></p>

**Boot diagnostics**
<p><img src="assets/screenshots/screen-boot.svg" width="480"></p>

---

## Building

Requires the Tanmatsu ESP-IDF toolchain. Clone the
[Tanmatsu template](https://github.com/Nicolai-Electronics/tanmatsu-template-pax)
first to set up `.IDF_PATH` and `.IDF_TOOLS_PATH`.

```sh
make build  DEVICE=tanmatsu       # produces build/tanmatsu/*.bin
make upload DEVICE=tanmatsu       # badgelink appfs upload — keeps the launcher
```

### Install with custom tile icon

`make upload` puts the binary in AppFS; the launcher shows a generic
"app" icon for AppFS entries. For the custom MeshCore tile icon (see
`assets/`), drop the bundle on SD instead:

```sh
SLUG=nl.cj.meshcore
BL=path/to/badgelink.sh

$BL fs mkdir   /sd/apps/$SLUG
$BL fs upload  /sd/apps/$SLUG/metadata.json  assets/metadata.json
$BL fs upload  /sd/apps/$SLUG/icon32.png     assets/icon-32.png
$BL fs upload  /sd/apps/$SLUG/meshcore.bin   build/tanmatsu/application.bin
```

The launcher's `create_list_of_apps_from_directory` reads `metadata.json`,
loads the 32×32 PNG into the tile, and registers `meshcore.bin` as the
executable. Same bundle is the artifact for a future appstore upload.

Full toolchain setup, partition layout, launcher patches, and C6 radio
firmware flashing are documented in
[Build / Deploy](docs/Build-Deploy.md).

---

## Documentation

| Page | About |
|---|---|
| [Architecture](docs/Overview.md) | Modules under `main/` and how they interact |
| [MeshCore protocol](docs/MeshCore-Protocol.md) | Packet types, ADVERT/DM/Channel/PATH, encryption, ACK |
| [UI / UX](docs/UI-UX.md) | Tabs, key bindings, edit-mode state machine, QR overlay |
| [Settings / NVS](docs/Settings-NVS.md) | Persistent keys, defaults, ranges, presets |
| [GPS sources](docs/GPS-Sources.md) | All 5 input paths, what's tested vs preview, how to wire OwnTracks / iOS Shortcuts / MeshMapper |
| [Sounds](docs/Sounds.md) | WAV format, recommended free sources, `ffmpeg` + `badgelink` setup |
| [SD card layout](docs/SD-Card-Layout.md) | `/sd/meshcore/`, encryption, self-heal |
| [C6 radio](docs/C6-Radio.md) | `lora_rpc`, RSSI/SNR patches, firmware update workflow |
| [Build / Deploy](docs/Build-Deploy.md) | IDF env, badgelink, launcher dependency, partition layout |

---

## Development write-up

Read about the development journey and lessons learned on Medium:
[Building a MeshCore Client on the Tanmatsu Badge](https://medium.com/@cjvansoest/building-a-meshcore-client-on-the-tanmatsu-badge-cfc46f02227f)

---

## Bug reports & feature requests

This is a community build of MeshCore for the Tanmatsu badge, **not the
official MeshCore iOS/Android app**. For issues, questions, or feature ideas
that are specific to this app, please open a ticket so they don't get lost in
chat threads:

→ **[github.com/CJvanSoest/meshcore/issues](https://github.com/CJvanSoest/meshcore/issues)**

If you're chatting on the MeshCore Discord and the question is specific to
the Tanmatsu app, please open a ticket here too — it makes the history
searchable for the next person hitting the same thing.

---

## License

MIT — see [LICENSE](LICENSE).

Developed by **CJ van Soest** with **[Claude AI](https://claude.ai)** (Anthropic)
as AI co-author. Claude assisted with protocol reverse engineering, cryptography
implementation, and UI development.

### Third-party components

| Component | Author | License |
|---|---|---|
| `qrcodegen.{c,h}` | Project Nayuki | MIT |
| `lodepng.{c,h}` | Lode Vandevenne | zlib |
| `ed25519.{c,h}` | NaCl/SUPERCOP ref10 (D.J. Bernstein et al.) + ESP32 adaptation | Public domain + MIT |
| `emoji_bitmaps.c` (Twemoji subset) | Twitter / jdecked | CC-BY 4.0 |
| `meshcore/` | Scott Powell / rippleradios.com, Nicolai Electronics | MIT |
| Badge BSP & template | Nicolai Electronics | MIT / CC0 |
| pax-gfx | robotman2412 / Badge.Team | MIT |
