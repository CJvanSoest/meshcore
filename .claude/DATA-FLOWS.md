# Data flows

How the firmware actually runs: the cold-start order, and the RX and TX paths
with the real function names. Use this to find where a symptom originates before
editing. Components are mapped in [COMPONENTS.md](COMPONENTS.md); crypto detail
is in [CRYPTO.md](CRYPTO.md).

## Cold start (`main/main.c` `app_main`)

In order:

1. `gpio_install_isr_service`, then `nvs_flash_init` (erase + retry on
   no-free-pages / new-version).
2. `bsp_device_initialize`, display parameters, `pax_buf_init` + orientation,
   `bsp_input_get_queue`.
3. Domain init: `nodes_init`, `chat_init`, `channels_init`, `identity_init`,
   `emoji_init`. **`identity_init` runs the RFC 8032 Ed25519 boot self-test and
   `abort()`s if it fails** (see [CRYPTO.md](CRYPTO.md)).
4. Splash render (title + community-app attribution).
5. `setenv("TZ", "CET-1CEST,...")` + `tzset`. The NVS epoch stays UTC.
6. `wifi_connection_init_stack` brings up the P4 to C6 SDIO RPC pipeline that
   `tanmatsu-lora` rides on. The actual WiFi connect is deliberately skipped:
   the badge is a LoRa node, not a WiFi client, so no scan / associate / DHCP /
   SNTP burns air.
7. `bsp_rtc_update_time` pulls the time the launcher already synced into the C6
   RTC. On success `identity_mark_time_synced()`. There is no app-side SNTP.
8. Fallback: if no RTC/SNTP, restore last known epoch from NVS.
9. RX/TX bring-up: `mc_rx_init()` registers the RX sink, `radio_start_tasks()`
   spins the LoRa RX + noise-floor tasks and creates `rx_mutex` + `dc_mutex`,
   `mc_rx_start_advert_task()` starts the advert broadcaster.
10. The UI event loop reads the input queue and renders.

## RX path (incoming LoRa packet to screen)

```
SX1262 -> tanmatsu-lora -> lora_rx_task (radio.c)
  deserialize bytes -> meshcore_message_t          (meshcore/packet.c)
  rx_is_duplicate(fp16)? drop                       (radio.c dedup ring)
  s_rx_sink(msg, meta)  == mc_rx_dispatch           (registered by mc_rx_init)
    switch (msg->type):
      ADVERT   -> rx_handle_advert  -> update_node / contact stats
      GRP_TXT  -> rx_handle_grp_txt -> brute-force channel keys -> chat
      TXT_MSG  -> rx_handle_dm      -> candidate-loop decrypt -> chat -> ACK
      PATH     -> rx_handle_path    -> candidate-loop ACK match -> chat_mark_ack
```

`radio.c` is pure transport: it deserializes, deduplicates, and hands the raw
`meshcore_message_t` to the sink. It never decrypts and never reads the domain.
All protocol logic lives in `mc_rx`.

Key detail: the RX task runs concurrently with the UI task. Anything it touches
in the domain (`node_list`, `contacts`, chat ring) must respect `node_mutex` /
`chat_mutex`. See the locking pitfalls in [PITFALLS.md](PITFALLS.md).

## TX path (compose to air)

```
UI / advert task / RX-task ACK -> mc_rx TX composer
  build meshcore_message_t (payload, route, path, bytes_per_hop)
  radio_tx_message(msg)                              (radio.c)
    apply_region_scope(msg)        -> region transport code (mc_crypto)
    meshcore_serialize(msg) -> bytes                 (meshcore/packet.c)
    airtime = compute_airtime_ms(len)
    dc_budget_available(airtime)?  -> no: drop, set dc_last_tx_blocked
    lora_send_packet(...)
    dc_record_tx(airtime)
```

`radio_tx_message` is the single shared TX primitive. Every send goes through
it, so the duty-cycle gate and region scope are applied once, centrally. It is
called from several tasks at once, which is why `dc_*` accounting is now under
`dc_mutex`.

## Advert flow

Both routes build the same signed advert through `send_advert_internal` (the
only `ed25519_sign` call). `direct_route` changes only `msg.route` and
`msg.path`, set after signing, so the signature is identical either way.

- `send_advert()` -> `send_advert_internal(false, ...)` -> FLOOD route.
- `send_advert_direct()` -> spawns `adv_direct` task -> one
  `send_advert_internal(true, dst_hash, ...)` per contact (max 16), 250 ms
  apart. Stock receivers drop a direct packet with no destination match, so we
  address one per contact by `sha256(pub_key)[0..bph-1]`.
- `advert_task` honours the flood + direct interval settings (0 = that schedule
  off) and re-reads them each second.

The signable byte range is built by the pure `meshcore_advert_signable_bytes`
(`mc_proto/advert_sign.c`): `pub_key[32] | timestamp[4] | tail` (skipping the
64-byte signature slot). See [CRYPTO.md](CRYPTO.md).

## DM flow

Send (`send_dm_message`):
```
mc_crypto_dm_encrypt(target_pub, node_prv_key, plain) -> ECDH secret -> AES + MAC
build payload: dst[1] | src[1] | mac[2] | ciphertext
radio_tx_message (route DIRECT)
```

Receive (`rx_handle_dm`):
```
dest_hash == us?  (else ignore)
for each candidate node whose pub_key[0] == src_hash (find_next_sender_by_hash):
    mc_crypto_dm_decrypt(payload, candidate_pub, node_prv_key) verifies the MAC?
        yes -> stop; this is the sender
chat_add_dm(...) ; contact_ensure (under node_mutex) ; notify
dm_send_path_return(...)   -> encrypted ACK (PATH_RETURN) back to sender
```

The candidate loop exists because the on-wire sender hash is only one byte and
collides. The MAC is the real disambiguator (the channel handler does the same
for channel keys). Do not collapse it back to a single-sender lookup.

ACK (`rx_handle_path`): a PATH_RETURN carries an encrypted inner block; for each
candidate sender, derive the ECDH secret, AES-decrypt, and accept when
`inner[1] == ACK` and the CRC matches one of our recent outgoing DMs
(`chat_mark_ack_by_crc`).

## Channel flow

Channels (GRP_TXT) are symmetric: a channel has a 16-byte AES key. Send encrypts
with `mc_crypto_grp_encrypt`; receive brute-forces every known channel key with
`mc_crypto_grp_decrypt` and accepts on the HMAC match. Channel slot 0 is the
fixed Public channel; it cannot be removed.

## Time and notifications

- Time: C6 RTC at boot, no SNTP. `last_seen_unix` is only stamped once
  `identity_sntp_synced()` is true; before that nodes carry `last_seen_unix == 0`
  and sort by the boot-relative `last_seen_ms`.
- LED: `update_notification_led` is driven by the live unread totals
  (`contact_unread_total` / `channel_unread_total`), not sticky flags, so it
  clears the instant the last unread item is opened.
