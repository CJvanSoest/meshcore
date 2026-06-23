# SD card layout

Chat history (DM and channel) is stored on the microSD card. The slot used
is the **internal SDMMC slot 0** (slot 1 hosts the WiFi link to the C6).

Mount point: `/sd`. Driver: `esp_vfs_fat_sdmmc_mount` with 4-bit SDMMC,
internal pull-ups, default frequency, no auto-format.

## Directory structure

```
/sd
└── meshcore/
    ├── channel.bin                       ← public-channel history
    ├── coverage/
    │   └── cov_<unix>.csv                ← Toolbox coverage-test session log
    ├── log/
    │   └── pkt_<unix>.csv                ← Toolbox packet-log export (E key)
    └── dm/
        ├── a3f2c91a8b7e4f5d.bin          ← per-peer DM log (16 hex chars
        ├── 7e29c84d1b0f6a52.bin             = first 8 bytes of peer pubkey)
        └── …
```

The filename is `<first 8 bytes of peer pub-key in hex>.bin` (16 chars).
With `MAX_CONTACTS = 16` the 64-bit prefix collision risk is negligible.

## Record format

Each record on disk:

```c
struct __attribute__((packed)) history_rec_hdr_t {
    uint8_t  magic[4];      // "MCR1"
    uint16_t plain_len;     // 1 .. MAX_MSG_TEXT
    uint8_t  flags;         // bit 0 = is_mine
    uint8_t  reserved;
    uint32_t ts_unix;       // seconds, little-endian
    uint8_t  iv[16];        // random IV for this record
};                          // 28 bytes
uint8_t ciphertext[ ceil(plain_len, 16) ];  // AES-128-CBC, PKCS#7
```

## Encryption

- **Cipher**: AES-128-CBC with PKCS#7 padding.
- **Key derivation**: at `history_init(prv_key)`,
  `key = HMAC-SHA256(prv_key, "mc-history-v1")[0:16]` (mbedTLS
  `mbedtls_md_hmac`).
- **IV**: 16 bytes from `esp_fill_random`, persisted in the header per
  record. No IV reuse across records.

The history key is therefore derived from the Ed25519 *identity* private
key. Wiping NVS regenerates the identity → previous on-disk records become
unreadable (see "Self-heal" below).

## Append path

`history_append_dm(peer_pub, text, is_mine)` and
`history_append_channel(text, is_mine)` both:

1. Take the `s_mutex` (history lock).
2. Compose the 28-byte header (magic, length, flags, current Unix time,
   random IV).
3. PKCS#7-pad and AES-CBC encrypt.
4. `fopen(path, "ab")` + `fwrite(hdr) + fwrite(ct)` + `fclose`.

The write is fsync'd by `fclose` semantics on the FAT driver. Power loss
mid-write loses at most the in-flight record.

## Load path

`history_load_dm(peer_pub, add)` and `history_load_channel(add)` read
sequentially from start of file. For each record:

- Verify magic bytes (`"MCR1"`).
- Verify `plain_len` in range.
- Read ciphertext, decrypt, verify PKCS#7 padding matches `plain_len`.
- Call the `add` callback with `text` and `is_mine`.

If a record fails the magic / length / padding checks the loop stops
(`break`). Records before the failure are still loaded.

## Self-heal on identity change

When the load loop bails out **at record 0** with a fatal error
(`bad magic` / `bad len` / `decrypt mismatch`), the entire file is removed:

```c
if (fatal && loaded == 0) {
    ESP_LOGW(TAG, "(%s): unreadable from start — removing stale file", path);
    remove(path);
}
```

This recovers automatically from "identity key changed" (e.g. after a full
NVS erase or partition table flash) — the next append starts a fresh log
under the new key instead of being shadowed forever by an unreadable old
log.

If only the *tail* of a file is corrupt, the good prefix is loaded and the
bad tail is left in place; subsequent appends will simply be added beyond
the corruption and only the bad records (and everything after them) are
lost on the next load. This is intentional — we avoid destroying readable
history on a single bit flip.

## Coverage-test logs (Toolbox)

The Toolbox coverage test writes one **plaintext CSV per session** under
`/sd/meshcore/coverage/cov_<unix>.csv` (the filename timestamp comes from the C6
RTC). Starting a new session (the `R` key, or re-entering the tool) opens a new
file with a header row; every ping attempt appends one row:

```
ts_unix,repeater,pubkey,lat_e6,lon_e6,attempt,reachable,rtt_ms,uplink_snr_db,downlink_snr_db
```

Unlike chat history these rows are **not encrypted**: they carry no message
content, only reachability telemetry (repeater name, 4-byte pubkey prefix, GPS
position when the fix is valid, attempt index, reachable 0/1, round-trip ms, and
the uplink/downlink SNR the TRACE returned). Owned by `mc_domain/coverage.c`,
which `mkdir`s the directory and appends with the same `fopen("ab")` pattern;
writing is a no-op when no card is mounted.

## Packet-log exports (Toolbox)

The Toolbox packet log (`VIEW_TOOLBOX_LOG`) exports the in-RAM diagnostics ring
to a **plaintext CSV** on the `E` key (`S` is the row-scroll key in that view).
Each press snapshots the ring — newest frame first — and writes one fresh file
`/sd/meshcore/log/pkt_<unix>.csv` (filename timestamp from the C6 RTC). A toast
reports the path and frame count, or the failure (no card / low memory). The
header and columns are:

```
ts_ms,dir,type,route,rssi_dbm,snr_db,len,raw_hex
```

`ts_ms` is capture time since boot, `dir` is `RX`/`TX`, `type`/`route` are the
decoded payload + route names (`?` if the header did not parse), `rssi_dbm` and
`snr_db` are blank on TX rows and on RX frames with no measurement, `len` is the
true on-air length, and `raw_hex` is the captured leading bytes (up to
`DIAG_RAW_MAX`) as lower-case hex. The row is formatted by the pure
`diag_csv_row()` in `mc_proto/diag_decode.c` (host-tested); the UI side in
`mc_ui/render_toolbox_log.c` `mkdir`s the directory and writes the file, a no-op
when no card is mounted. These rows carry on-air frame bytes only, no decrypted
message content.

## Locking

A single `s_mutex` (FreeRTOS binary semaphore) serialises all SD access
within `history.c`. Holds are kept short (one append or one full load).
`coverage.c` has its own mutex for its result table + log writes.

## Disabled / missing SD

If `mount_sd` fails the module sets `s_status` to one of:

| Status | Cause |
|---|---|
| `ok` | mounted successfully |
| `no-sd` | `ESP_ERR_NOT_FOUND` / `ESP_ERR_TIMEOUT` |
| `err` | other mount error |
| `off` | initial state, before `history_init` was called |

Append/load become no-ops when `s_ready == false`, so the rest of the app
keeps running without persistence.
