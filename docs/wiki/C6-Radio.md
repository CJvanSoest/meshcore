# C6 radio communication

The Tanmatsu has two ESP32 SoCs: the **ESP32-P4** runs the app + display,
and the **ESP32-C6** drives the **SX1268** LoRa transceiver. The P4 talks
to the C6 over a hosted-WiFi-like SDIO/SPI link via Nicolai Electronics'
`tanmatsu-lora` IDF component (which exposes `lora.h`).

## RPC surface (`lora.h`)

| Function | Purpose |
|---|---|
| `lora_init(qsize)` | Bring up the RPC link, allocate RX queue |
| `lora_get_config(&cfg)` | Read C6-side config (frequency, SF, BW, CR, power, sync, preamble) |
| `lora_set_config(&cfg)` | Push P4 config to C6 — applied immediately |
| `lora_set_mode(MODE)` | `RX`, `TX`, `SLEEP` |
| `lora_send_packet(buf, len)` | Enqueue a TX |
| `lora_recv_packet(...)` | Pop the next received frame |

`radio.c` builds an RX task that loops on `lora_recv_packet`. RSSI/SNR
fields are populated only when the firmware on the C6 supports them — see
below.

## RSSI / SNR + noise floor

Per-packet RSSI/SNR and the instant-RSSI (noise floor) poll are **upstream**
since `tanmatsu-lora` v0.2.1 / `tanmatsu-radio` (our PRs #14 / #3). The
component returns RSSI/SNR in the per-packet stats on every `PACKET_RX`, and
`lora_get_rssi_inst(handle, float* dBm)` polls the instant RSSI for the
noise-floor display (60 s interval — 5 s breaks the SX126x preamble→header
transition).

We run the C6 from a small Gitea fork of `tanmatsu-radio` (upstream main /
v3.1.0-line) carrying three not-yet-upstream patches:

1. `rx_boost` — boosted RX gain toggle (in the `tanmatsu-lora` fork).
2. `CONFIG_ESP_HOSTED_MAX_CUSTOM_MSG_HANDLERS` 3 → 8, so the LoRa protocol
   server actually registers (upstream registers 6 servers but the table is 3).
3. Fix for an inverted RX-forward check (`lora_receive_packet` returns
   `esp_err_t`, so `if (...)` was true only on *failure* — packets never
   reached the host).

Patches 2 and 3 are reported upstream in
[tanmatsu-radio#18](https://github.com/Nicolai-Electronics/tanmatsu-radio/issues/18);
drop the fork once they land.

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

## Frequency / band

Default is the EU 868 ISM band:

| | |
|---|---|
| Default frequency | 869.618 MHz (MeshCore canonical) |
| Band | EU 868 (863–870 MHz allowed) |
| Channel BW | typically 62 kHz, configurable 7–500 kHz |
| TX power | up to 22 dBm (chip max; legal cap is lower in EU) |

US, JP, AU and other bands work too — set `frequency` to the matching ISM
band frequency. The chip itself is multi-band; the Tanmatsu RF front-end is
optimised for 868/915 MHz.

## Firmware update workflow

**End users — update via the Launcher:** open the **Launcher → Tools →
Firmware update**. This pulls the latest stable launcher *and* C6 radio
firmware from `ota.tanmatsu.cloud` in one go. As of `tanmatsu-radio`
v3.1.1 the OTA bundle ships the `lora_protocol` server (callback-limit
fix) and the `tanmatsu-lora` v0.2.1 component MeshCore expects.

> **Do this before installing MeshCore on a freshly-recovered device.**
> `recovery.tanmatsu.cloud` currently serves an old radio firmware
> (`ESP-HOSTED 2.1.0`, pre-`tanmatsu-radio`-rename) and launcher 0.1.2 —
> too old for the MeshCore protocol: `lora_get_config` returns a
> response shorter than the 24/25-byte forms our fallback handles, and
> the app shows "LoRa radio not available". The launcher's "Tools →
> Firmware update" path is what unblocks it.

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

The Tanmatsu launcher checks the C6 firmware version against its
embedded expected version. We run a Gitea fork of `tanmatsu-radio`
(upstream main / v3.1.0-line + the three patches above), so the version
string is `v3.1.1-1-gf919f91` instead of an exact `v3.1.1`. Our
launcher fork prefix-matches `v3.1.` and hides both the warning and the
"Update radio" tile; on a stock launcher the warning may appear.
**Ignore it.** **Do not click "Update Radio"** on the home screen — it
hits `ota.tanmatsu.cloud/radio2/instructions.json` and currently
downgrades to an older stock build.
