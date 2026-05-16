# MeshCore for Tanmatsu

A [MeshCore](https://meshcore.co.uk) LoRa mesh communication app for the
[Tanmatsu](https://tanmatsu.cloud) badge (ESP32-P4 + ESP32-C6 radio).

## Features

- **DM** — encrypted direct messages to any node in the mesh (AES-128-ECB,
  ECDH key exchange via Ed25519 → Curve25519)
- **Channel** — public channel chat (AES-ECB with the public channel key)
- **Nodes** — live node list with role, name, packet count, advert age, and
  optional GPS distance; tap a node to open a DM conversation
- **Settings** — LoRa radio parameters (frequency, SF, BW, CR, TX power,
  sync word, preamble); stored in NVS and pushed to the C6 radio co-processor
- **QR code** — share your MeshCore public key / contact URL via QR overlay
- **Message LED** — bottom-left LED lights green (DM) or blue (channel) on
  incoming messages; clears when the relevant tab is opened
- **SNTP** — real Unix timestamps on outgoing messages

## Protocol notes

MeshCore ACK for FLOOD DMs is a **PATH_RETURN packet** (type 0x08), not a
bare ACK. Inner payload (16 bytes, AES-128-ECB encrypted with the shared
secret):

```
path_len=0x00 | extra_type=0x03 (ACK) | ack_crc[4] | zeros[10]
```

ACK CRC = `SHA256(timestamp[4] | flags[1] | text[n] | sender_pub[32])[0:4]`

HMAC for DM packets is tried in multiple variants (with/without
Edwards→Montgomery conversion, 32- and 16-byte key) to handle differences
between MeshCore versions.

## Building

Requires the Tanmatsu ESP-IDF toolchain (ESP-IDF v5.5.1 pinned locally).

```sh
IDF_PATH=$(cat .IDF_PATH) IDF_TOOLS_PATH=$(cat .IDF_TOOLS_PATH) \
  bash -c 'source "$IDF_PATH/export.sh" >/dev/null 2>&1 && \
  idf.py -B build/tanmatsu build \
    -DDEVICE=tanmatsu \
    -DSDKCONFIG_DEFAULTS="sdkconfigs/general;sdkconfigs/tanmatsu" \
    -DSDKCONFIG=sdkconfig_tanmatsu \
    -DIDF_TARGET=esp32p4 \
    -DFAT=0'
```

## License

MIT — see [LICENSE](LICENSE).

Developed by **CJ van Soest** with **Claude AI** (Anthropic) as AI
co-author. Claude assisted with protocol reverse engineering, crypto
implementation, and UI development throughout this project.

### Third-party components

| Component | Author | License |
|---|---|---|
| `qrcodegen.{c,h}` | Project Nayuki | MIT |
| `ed25519.{c,h}` | NaCl/SUPERCOP ref10 (D.J. Bernstein et al.) + ESP32 adaptation | Public domain + MIT |
| `meshcore/` | Scott Powell / rippleradios.com, Nicolai Electronics | MIT |
| Badge BSP & template | Nicolai Electronics | MIT / CC0 |
