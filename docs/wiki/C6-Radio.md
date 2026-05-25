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

## RSSI / SNR patches

The stock `tanmatsu-radio` v2.12.3 does not surface per-packet RSSI / SNR
to the host. We run a **patched build** that:

1. Reads RSSI/SNR registers after each SX1268 RX_DONE IRQ
2. Stores them in the per-packet metadata
3. Returns them via the existing `lora_recv_packet` struct

The patch lives in a local checkout (not an upstream PR) and must be
re-applied after upstream releases.

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

The Settings tab `U` key triggers `enter_radio_bootloader()` which:

1. Stops the ESP-Hosted heartbeat
2. Deinits ESP-Hosted
3. Powers the C6 OFF, then back ON in `BOOTLOADER` mode
4. Replaces the screen with the esptool command sheet

Then on the Mac:

```sh
esptool.py --chip esp32c6 --port /dev/cu.usbmodemXXXXX --before no_reset \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x0     build/tanmatsu/bootloader/bootloader.bin \
  0x8000  build/tanmatsu/partition_table/partition-table.bin \
  0xd000  build/tanmatsu/ota_data_initial.bin \
  0x10000 build/tanmatsu/tanmatsu-radio.bin
```

`--before no_reset` is critical — the C6 is already in DFU, we don't want
esptool to toggle the reset line. After flashing, press ESC / F1 to power
cycle the badge. On the next boot MeshCore detects the blank C6 NVS
(`frequency = 0`) and re-pushes the saved LoRa config.

## Mismatch warning in launcher

The Tanmatsu launcher checks the C6 firmware version against its embedded
expected version. We ship `tanmatsu-radio` v2.12.3 (+RSSI/SNR patches)
which is *newer* than the launcher's expected v2.7.3, so it prints a
"mismatch firmware" warning. **Ignore it** — the MeshCore app talks to
the radio directly and works fine. **Do not click "Update Radio"** in the
launcher; that downgrades the C6.
