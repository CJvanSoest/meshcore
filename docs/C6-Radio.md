# C6 radio communication

The Tanmatsu has two ESP32 SoCs: the **ESP32-P4** runs the app + display,
and the **ESP32-C6** drives the **SX1262** LoRa transceiver. The P4 talks
to the C6 over a hosted-WiFi-like SDIO/SPI link via Nicolai Electronics'
`tanmatsu-lora` IDF component (which exposes `lora.h`).

**Standard radio firmware: `tanmatsu-radio` v3.1.1** (stock upstream).
That's what the Launcher's Tools → Firmware update path installs, and
what MeshCore expects. The on-device Information tab shows the live
version via the v3.1.1 system-protocol `get_information` query.

## RPC surface (`lora.h`)

| Function | Purpose |
|---|---|
| `lora_init(qsize)` | Bring up the RPC link, allocate RX queue |
| `lora_get_config(&cfg)` | Read C6-side config (frequency, SF, BW, CR, power, sync, preamble) |
| `lora_set_config(&cfg)` | Push P4 config to C6 — applied immediately |
| `lora_set_mode(MODE)` | `RX`, `SLEEP` (TX is implicit via `lora_send_packet`) |
| `lora_send_packet(buf, len)` | Enqueue a TX |
| `lora_recv_packet(...)` | Pop the next received frame |
| `lora_get_rssi_inst(handle, float* dBm)` | Instant-RSSI poll for the noise-floor display |

`radio.c` builds an RX task that loops on `lora_recv_packet`. RSSI/SNR
fields are populated in the per-packet stats on every `PACKET_RX`.

## RSSI / SNR + noise floor

Per-packet RSSI/SNR are upstream since `tanmatsu-lora` **v0.2.1** /
`tanmatsu-radio` **v3.1.0** (originally our PRs #3 / #14). The component
returns RSSI/SNR in the per-packet stats on every `PACKET_RX`, and
`lora_get_rssi_inst` polls the instant RSSI for the noise-floor display
(60 s interval — 5 s breaks the SX126x preamble→header transition).

In the Nodes tab the RSSI column is coloured:

| RSSI | Colour |
|---|---|
| ≥ -80 dBm | Green |
| -80 .. -105 dBm | Amber |
| < -105 dBm | Red |

SNR (in dB):

| SNR | Colour |
|---|---|
| ≥ 0 dB | Green |
| -10 .. 0 dB | Amber |
| < -10 dB | Red |

## RX sensitivity (`rx_boost`)

Settings → RX sensitivity toggles SX1262 RxGain register `0x08AC` between
the "boosted" mode (`0x96`, ~+3 dB sensitivity, ~+2 mA RX current) and
power-save (`0x94`). In the field the boosted mode yields noticeably
more decoded weak-signal packets at the cost of a marginal RX-current
increase. The setting persists in NVS (`lora.rxboost`, default true) and
is reapplied on every `lora_set_config`.

Boost is part of the lora-config struct since `tanmatsu-lora` **v0.3.0**
(originally our PR #5; the upstream tag was cut 2026-06-03). The struct
is the 17-byte form; v0.2.1 used a 16-byte form without `rx_boost`. The
app accepts both response lengths (see "Firmware update workflow" below)
so the same build keeps working across the bump.

## Frequency / band

Default is the EU 868 ISM band:

| | |
|---|---|
| Default frequency | 869.618 MHz (MeshCore canonical) |
| Band | EU 868 (863–870 MHz allowed) |
| Channel BW | typically 62 kHz, configurable 7–500 kHz |
| TX power | up to 22 dBm (chip max; legal cap is lower in EU) |

US, JP, AU and other bands work too — set `frequency` to the matching ISM
band frequency. The SX1262 itself is multi-band; the Tanmatsu RF
front-end is optimised for 868/915 MHz.

## Firmware update workflow

**End users — update via the Launcher:** open the **Launcher → Tools →
Firmware update**. This pulls the latest stable launcher *and* the C6
radio firmware from `ota.tanmatsu.cloud` in one go. The OTA bundle
ships `tanmatsu-radio` v3.1.1 + the `tanmatsu-lora` component MeshCore
expects.

> **Do this before installing MeshCore on a freshly-recovered device.**
> `recovery.tanmatsu.cloud` currently serves an old radio firmware
> (`ESP-HOSTED 2.1.0`, pre-`tanmatsu-radio`-rename) and launcher 0.1.2 —
> too old for the MeshCore protocol: `lora_get_config` returns a
> response shorter than the 24/25-byte forms the app's fallback handles,
> and MeshCore reports "LoRa radio not available". The Launcher's
> Tools → Firmware update path is what unblocks it.

The app's response-length fallback accepts both:
- **24 bytes** = `lora_protocol_config_params_t` 16-byte form (`tanmatsu-lora` v0.2.1)
- **25 bytes** = 17-byte form including `rx_boost` (`tanmatsu-lora` v0.3.0+)

so the same build runs on both stock v3.1.1 (currently shipping v0.2.1)
and on a future radio bundle that picks up v0.3.0.

**Developers — flash a custom C6 build via esptool:** put the C6 into
DFU mode externally (radio off → radio on into bootloader via the BSP
power APIs) and flash with:

```sh
esptool.py --chip esp32c6 --port /dev/cu.usbmodemXXXXX --before no_reset \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x0     build/tanmatsu/bootloader/bootloader.bin \
  0x8000  build/tanmatsu/partition_table/partition-table.bin \
  0x10000 build/tanmatsu/tanmatsu-radio.bin
```

> The current `tanmatsu-radio` partition table dropped the unused
> OTA-data partition (no `0xd000 ota_data` line) and grew the app
> partition. Always take the exact offsets from
> `build/tanmatsu/flasher_args.json`.

`--before no_reset` is critical — the C6 is already in DFU, we don't
want esptool to toggle the reset line. On the next boot MeshCore
detects the blank C6 NVS (`frequency = 0`) and re-pushes the saved
LoRa config.

## Mismatch warning in launcher

The Tanmatsu launcher checks the live C6 version string against an
embedded expected version. **Stock setup (launcher v0.1.6 + radio
v3.1.1) matches exactly → no warning, no "Update radio" tile.** The
warning only appears when one side runs an off-tag build — for example
a developer running a git-described radio (`v3.1.1-1-g<sha>`) or a
launcher that doesn't yet recognise the radio's tag. If you see it on
a stock device, *don't* press "Update Radio" on the home screen — that
hits `ota.tanmatsu.cloud/radio2/instructions.json` which currently
points to an older bundle. Use Tools → Firmware update instead.

See [Firmware Versions](Firmware-Versions.md) for the precise upstream
versions each piece tracks, and where we still carry deltas.
