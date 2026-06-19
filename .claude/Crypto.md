# Crypto model

Everything an AI contributor must understand before touching anything that
signs, encrypts, decrypts, or derives a key. Getting this wrong ships a device
that is silently rejected by the rest of the mesh, and the sender never sees the
rejection, so it hid for months once already. The components are `mc_crypto`
(channel), `mc_crypto_dm` (DM), and `components/vendor/ed25519*`.

## Identity

A node identity is an Ed25519 key pair derived from a 32-byte seed in NVS
(`identity.c`). The 32-byte public key is the node's address; its first byte is
the 1-byte "hash" used on the wire to narrow a sender (the full key is the real
identity, the byte only narrows). The 64-byte private key is the expanded form
(scalar + nonce prefix).

## The ed25519 split (read this twice)

`components/vendor/` ships TWO translation units and they divide by symbol, not
by "main vs variant":

| Symbol | Defined in | Used by |
|---|---|---|
| `ed25519_sign`, `ed25519_create_keypair` | **`ed25519_mpi.c`** (mbedtls_mpi rewrite) | advert signing, identity, the boot self-test |
| `ed25519_key_exchange`, `ed25519_key_exchange_raw`, `ed25519_pub_to_x25519` | **`ed25519.c`** (ref10) | DM ECDH (`mc_crypto_dm.c`) |

`ed25519.c` does not contain the string `ed25519_sign` at all. The old ref10
signer produced wrong points for the RFC 8032 vector, which is why direct
adverts looked dead; the fix was to define the signer only in `ed25519_mpi.c`.
So calling `ed25519_sign` resolves to the correct implementation. There is no
`ed25519_mpi_sign`. Do not delete `ed25519_mpi.c` as "unused" (callers reference
the bare symbol, not the filename) and do not delete `ed25519.c` (its X25519
path is in use). Both ship. This is also in [Pitfalls.md](Pitfalls.md).

## Advert signing

The signature covers `pub_key[32] | timestamp[4] | tail`, where the tail is
everything after the 64-byte signature slot (flags, name, optional position).
The signature slot itself is skipped. The pure helper
`meshcore_advert_signable_bytes` (`mc_proto/advert_sign.c`) builds exactly that
byte range so it can be tested without the radio; `mc_rx`'s
`send_advert_internal` then calls `ed25519_sign` over it. Both flood and direct
adverts sign identical bytes.

Two gates protect this and you keep both green:
- `test_ed25519` checks the signer against the RFC 8032 vectors on the host.
- `test_advert_sign` checks the signable byte layout (offset + golden vector).
- At runtime `identity_init` re-runs the RFC 8032 TV1 keypair + sign and
  `abort()`s on mismatch, so a broken build refuses to boot rather than
  transmitting rejected signatures silently.

## Channel crypto (GRP_TXT)

Symmetric. Each channel has a 16-byte AES-128 key.
- Encrypt (`mc_crypto_grp_encrypt`): AES-128-ECB over the zero-padded plaintext
  (`timestamp[4] | text_type[1] | text`, padded to a 16-byte multiple), then
  `mac = HMAC-SHA256(key, ciphertext)[0..1]` (the on-wire MAC is 2 bytes).
- Decrypt (`mc_crypto_grp_decrypt`): recompute the HMAC, compare the first
  `MESHCORE_CIPHER_MAC_SIZE` (2) bytes, reject on mismatch, then AES-decrypt.
- RX brute-forces every known channel key and accepts on the MAC match, because
  the wire carries no key id.

The 2-byte MAC means a wrong key passes about 1/65536 of the time. That is the
upstream MeshCore design, not a defect; do not "fix" it by widening the MAC (it
is a wire-format value).

## DM crypto (TXT_MSG)

Asymmetric setup, symmetric payload.
- Shared secret: `ed25519_key_exchange*` turns our private key + the peer's
  public key into an X25519 ECDH secret. There are two variants (converted and
  raw) for interop with different upstream clients.
- Encrypt (`mc_crypto_dm_encrypt`): AES-128 over the padded plaintext, MAC via
  HMAC over the ciphertext, payload `dst[1] | src[1] | mac[2] | ciphertext`.
- Decrypt (`mc_crypto_dm_decrypt`): tries 4 HMAC combinations (converted vs raw
  secret, 16- vs 32-byte HMAC key) against the 2-byte MAC, then AES-decrypts on
  the first that matches. `*out_text_len = ct_len - 5` (the `timestamp[4] |
  flags[1]` header rides inside).

The DM RX handler tries every node whose pubkey first byte matches the 1-byte
sender hash and lets the MAC pick the real sender (see [Data-Flows.md](Data-Flows.md)).

## Region transport code

`mc_crypto_region_transport_code(region, type, payload, len)` derives the
2-byte transport code that scope-aware relays match on:
`HMAC-SHA256(SHA256("#" + region)[0:16], type || payload)[0:2]`, with `0x0000`
and `0xFFFF` remapped (reserved sentinels). The `#` prefix is mandatory: upstream
`RegionMap::getTransportKeysFor` prepends it, and omitting it makes relays
compute a different code and drop the packet. This caused a real interop bug; it
is covered by `test_mc_crypto`'s region test (spec match + `#`-prefix
invariance). `radio.c` applies it in `apply_region_scope` during TX.

## ACK CRC

`mc_crypto_ack_crc(head5, text, len, pubkey, out_crc)` =
`SHA256(head5 || text || pubkey)[0:4]`. It binds the ACK to the sender's pubkey
so a PATH_RETURN can be matched to the exact outgoing DM. Covered by
`test_mc_crypto`.

## Rules when you touch crypto

- Keep the pure crypto in `mc_crypto` / `mc_crypto_dm` and add a host vector for
  anything you change. The expensive ECDH must run outside any mutex.
- Never change a wire-format size or layout locally. It goes upstream first.
- If you touch the signer or the signable layout, both `test_ed25519` and
  `test_advert_sign` must stay green, and the boot self-test must still pass.
