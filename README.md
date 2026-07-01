# MeshCore for Tanmatsu

[![Build and test](https://github.com/CJvanSoest/meshcore/actions/workflows/ci.yml/badge.svg)](https://github.com/CJvanSoest/meshcore/actions/workflows/ci.yml)

A [MeshCore](https://meshcore.co.uk) LoRa mesh chat app for the
**[Tanmatsu](https://tanmatsu.cloud) badge**, fully interoperable with the
MeshCore iOS/Android app: encrypted direct messages and public channel chat
over LoRa.

<p>
  <img src="assets/screenshots/screen-home.svg" width="400" alt="Home">
  <img src="assets/screenshots/screen-nodes.svg" width="400" alt="Nodes">
</p>
<p>
  <img src="assets/screenshots/screen-dm.svg" width="400" alt="DM conversation">
  <img src="assets/screenshots/screen-channel.svg" width="400" alt="Channel">
</p>

_All views: [screenshots](docs/features/Screenshots.md)._

## Features

- **End to end encryption** — Ed25519 identity, DMs over ECDH + AES, channels
  over a shared key.
- **DMs, channels and contacts** — per contact conversations with delivery ACKs,
  user added `#channels`, and encrypted chat history on SD.
- **On-device map and GPS** — a slippy OSM map with a live position from any of
  five sources.
- **Regulatory helper** — per country frequency and power limits with hard duty
  cycle enforcement.
- **More** — HTTPS with a self generated cert, a BLE companion radio,
  notification sounds, and a QR contact card.

## Quick start

**Install** on a recovered Tanmatsu: open the launcher, run **Tools → Firmware
update** (this pulls the radio firmware MeshCore needs), then install
**MeshCore** from the appstore.

**Build** it yourself (needs the Tanmatsu ESP-IDF toolchain):

```sh
make build  DEVICE=tanmatsu      # build/tanmatsu/*.bin
make upload DEVICE=tanmatsu      # badgelink AppFS upload, keeps the launcher
```

## Documentation

Everything beyond this page lives in [`docs/`](docs/README.md).

| Page | About |
|---|---|
| [Getting started](docs/guides/Getting-Started.md) | First run: set your Owner + Advert name, then get on the air |
| [Blueprint](docs/architecture/Blueprint.md) | Design rationale and how to program in this project |
| [Architecture](docs/architecture/Architecture.md) | The enforced layers, forbidden includes, wire boundary |
| [Overview](docs/architecture/Overview.md) | The `mc_*` components and how they interact |
| [Protocol](docs/reference/MeshCore-Protocol.md) | Packet types, encryption, ACK |
| [UI / UX](docs/features/UI-UX.md) | Views, key bindings, edit-mode state machine |
| [Settings / NVS](docs/reference/Settings-NVS.md) | Persistent keys, defaults, regulatory tables |
| [GPS sources](docs/features/GPS-Sources.md) | The four input paths and how to wire them |
| [Maps](docs/features/Maps.md) · [Sounds](docs/features/Sounds.md) · [SD layout](docs/reference/SD-Card-Layout.md) | Feature detail |
| [C6 radio](docs/reference/C6-Radio.md) · [Build / Deploy](docs/guides/Build-Deploy.md) | Hardware and toolchain |

## Contributing

The app is split into ESP-IDF components with a compiler enforced dependency
graph; pure logic is host tested off device. Start with
[docs/architecture/Blueprint.md](docs/architecture/Blueprint.md) and the
[contributor checklist](docs/guides/CONTRIBUTING.md). AI pair programmers: the same
model is in [`.claude/`](.claude). Issues and ideas go to
[the tracker](https://github.com/CJvanSoest/meshcore/issues). This is a
community build, not the official MeshCore app.

## License

MIT, see [LICENSE](LICENSE). Third-party components and their licences are in
[AUTHORS](AUTHORS).

Developed by **CJ van Soest** with **[Claude AI](https://claude.ai)** (Anthropic)
as AI co-author.
