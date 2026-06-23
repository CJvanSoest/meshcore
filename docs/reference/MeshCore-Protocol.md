# MeshCore protocol notes

The MeshCore protocol is implemented here against the
[upstream C library](https://github.com/ripplebiz/MeshCore) and confirmed
interoperable with the iOS/Android client.

## Packet types observed

| Type | Direction | Purpose |
|---|---|---|
| `0x02` TXT_MSG | TX & RX | Encrypted DM (AES-128-ECB inner, ECDH outer) |
| `0x03` ACK | RX | Delivery acknowledgement (carried inside a PATH return) |
| `0x04` ADVERT | TX & RX | Node identity broadcast |
| `0x05` GRP_TXT | TX & RX | Public channel chat (AES-128-ECB, shared key) |
| `0x08` PATH | RX | Returned by the recipient; carries the ACK |
| `0x09` TRACE | TX & RX | Repeater reachability probe; accumulates a per-hop SNR path (Toolbox coverage test) |

These are payload types (`meshcore_payload_type_t` in `meshcore/packet.h`).
`DIRECT` is a **route** type (`MESHCORE_ROUTE_TYPE_DIRECT = 0x2`), not a payload
type: a DM is a `TXT_MSG` payload sent on the `DIRECT` route.

## TRACE (reachability probe)

`mc_proto/trace.{c,h}` (pure, host-tested). Payload =
`tag[4] | auth[4] | flags[1] | hop_hashes[...]`; `flags & 0x03` is `path_sz`
(hop-hash size: `1 << path_sz` bytes). The Toolbox coverage test sends one to a
repeater's pubkey-prefix hash and matches the return by `tag`.

Wire gotchas that are easy to get wrong (see
[Toolbox.md](../features/Toolbox.md)):

- **Route is plain `DIRECT`, unscoped** (no transport codes). Upstream only
  handles a TRACE on a direct route and refuses to flood it; repeaters
  region-gate flood packets only, so a direct trace is forwarded regardless of
  scope. Never wrap a TRACE in `TRANSPORT_FLOOD`/`TRANSPORT_DIRECT`.
- **Path-control size is 1** (control byte `0x00`). The wire path field is the
  per-hop SNR accumulator (one byte per hop); the hop-hash size lives in the
  payload `flags`, not the path-control byte. Encoding the hash size there
  (`0x40`) makes a repeater read `path_len = 64` and abandon the probe.

## ADVERT

`meshcore/payload/advert.h` packs:

- `pub_key[32]` — Ed25519 identity
- `name` (UTF-8, ≤ `MESHCORE_MAX_NAME_SIZE`)
- `role` — `CHAT_NODE`, `REPEATER`, `ROOM_SERVER`, `SENSOR`
- Timestamp + signature

`lora_advert_name` overrides `owner_name` if set; otherwise owner is used. The
QR code in the Nodes tab encodes the same `name` so a scanned contact matches
incoming adverts.

## DM encryption

1. Derive ECDH shared secret: convert peer Ed25519 pub key → Curve25519, then
   X25519 with our converted private key.
2. AES-128-ECB encrypt plaintext with the derived key.
3. Wrap in DIRECT packet with our pub key, recipient pub key prefix, timestamp.

Decryption tries **multiple HMAC variants** (with/without Edwards→Montgomery
conversion, 32- and 16-byte key) — different MeshCore versions historically
generated the shared secret slightly differently, so this fallback keeps us
compatible.

## ACK mechanism

MeshCore replies with a **PATH_RETURN packet** (`0x08`) carrying an ACK inside
the encrypted payload — there is no bare ACK packet type.

Inner payload (16 bytes, AES-128-ECB encrypted):

```
path_len=0x00 | extra_type=0x03 (ACK) | ack_crc[4] | zeros[10]
```

`ack_crc = SHA256(timestamp[4] | flags[1] | text[n] | sender_pub[32])[0:4]`

To detect a matching ACK we compute the CRC over our own outgoing DM and
match it against the `ack_crc` field of incoming PATH_RETURN payloads.

## PATH and `path_hash_size`

A `path_hash_size` setting controls how many bytes of node-pub-key prefix
are used in routing tables — affects max hop count vs. collision probability:

| Bytes | Max hops | Trade-off |
|---|---|---|
| 1 | 64 | Most hops, more prefix collisions |
| 2 | 32 | Default-ish |
| 3 | 21 | Fewest collisions, fewest hops |

Setting is advertised in ADVERT and used for outgoing PATH packets.

## Channel chat

Public channel uses a static shared key, AES-128-ECB, no per-user ECDH.
Anyone with the channel key can read and write — typical chat-room behaviour.
Plaintext is `GRP_TXT` payload, optionally with the sender name prefix
(`MeshRelay-1: hello`).

## Timestamps

Every outgoing packet includes a Unix timestamp (seconds). The receiving
client uses this to render message time. **SNTP sync matters**: without it
the receiving phone shows messages dated 1970 or whatever the NVS fallback
provided.
