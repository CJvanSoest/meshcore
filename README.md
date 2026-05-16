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
| **Radio chip** | SX1268 (LoRa, 868 MHz EU band) |
| **Display** | 4" MIPI DSI, 800×1280 px |
| **Framework** | ESP-IDF v5.5.1 |

The Tanmatsu is an open-source badge developed by
[Nicolai Electronics](https://tanmatsu.cloud). More information and hardware
schematics are available on the Tanmatsu documentation site.

---

## Features

### Tabs

| Tab | Description |
|---|---|
| **Settings** | Configure LoRa radio parameters; stored in NVS and synced to C6 |
| **Nodes** | Live list of heard MeshCore nodes — role, name, packet count, last seen |
| **DM** | End-to-end encrypted direct messages; select a node in Nodes tab to start |
| **Channel** | Public channel chat (AES-ECB, shared channel key) |

### Highlights

- **Full MeshCore interoperability** — compatible with MeshCore iOS/Android app
  (confirmed working: DM send/receive, delivered acknowledgement, channel chat)
- **End-to-end encryption** — Ed25519 keypair generated on first boot, stored
  in NVS; DMs encrypted with ECDH (Ed25519 → Curve25519) + AES-128-ECB
- **QR contact sharing** — press Q in Nodes tab to show a QR code that can be
  scanned directly by the MeshCore app to add you as a contact
- **Message LED** — bottom-left LED on the badge lights up for incoming
  messages: green = DM, blue = channel; clears when the tab is opened
- **Real timestamps** — SNTP synchronisation via WiFi for correct message times
  on the receiving iOS/Android device

### Controls

| Key | Action |
|---|---|
| Tab | Cycle through tabs |
| Enter (on node) | Open DM conversation with that node |
| T | Start typing a message (DM or Channel tab) |
| W / S | Scroll up / down |
| A | Send advertisement (announce yourself to the mesh) |
| Q | Show QR code (Nodes tab) |
| U | Put C6 radio into bootloader mode (for firmware update) |
| F1 / Red X | Exit to launcher |

---

## Screenshots

*(Coming soon)*

---

## Building

Requires the Tanmatsu ESP-IDF toolchain. Clone the
[Tanmatsu template](https://github.com/Nicolai-Electronics/tanmatsu-template-pax)
first to set up `.IDF_PATH` and `.IDF_TOOLS_PATH`.

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

---

## Technical notes

### ACK mechanism
MeshCore sends a **PATH_RETURN packet** (type `0x08`) as DM acknowledgement,
not a bare ACK. Inner payload (16 bytes, AES-128-ECB encrypted):

```
path_len=0x00 | extra_type=0x03 (ACK) | ack_crc[4] | zeros[10]
```

ACK CRC = `SHA256(timestamp[4] | flags[1] | text[n] | sender_pub[32])[0:4]`

### HMAC variants
DM decryption tries multiple HMAC variants (with/without Edwards→Montgomery
conversion, 32- and 16-byte key) for compatibility across MeshCore versions.

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
| `ed25519.{c,h}` | NaCl/SUPERCOP ref10 (D.J. Bernstein et al.) + ESP32 adaptation | Public domain + MIT |
| `meshcore/` | Scott Powell / rippleradios.com, Nicolai Electronics | MIT |
| Badge BSP & template | Nicolai Electronics | MIT / CC0 |
