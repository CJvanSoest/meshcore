# Testing

How the host test suite works, what it does and does not prove, and how to add a
test. The harness exists so a regression goes red in CI before any IDF build,
and before any hardware. The workflow that uses it is in [WORKFLOW.md](WORKFLOW.md).

## What the harness is

Plain host gcc tests under `tests/`, built and run by `tests/Makefile`. They
need only `gcc`, `libmbedtls-dev` and `cppcheck`. No ESP-IDF, no board.

```sh
cd tests && make test
```

The defining property: **each test links the real shipping `.c` files**, not a
copy. `test_mc_crypto` links the same `mc_crypto.c` that ships; `test_ed25519`
links `ed25519_mpi.c`, the actual signer. A passing test therefore proves the
firmware code is correct, not a parallel reimplementation. This is why pure
logic is kept free of ESP-IDF headers: so it can be linked on the host.

## The tests and what each guards

| Test | Links | Guards |
|---|---|---|
| `test_ed25519` | `ed25519_mpi.c` | RFC 8032 keypair + sign vectors (the shipping signer) |
| `test_advert_sign` | `advert_sign.c`, `advert.c`, `ed25519_mpi.c` | advert signable-byte layout (offset + golden signature) |
| `test_mc_crypto` | `mc_crypto.c` | channel encrypt/decrypt, wrong-key reject, tamper reject, ACK CRC, region transport code |
| `test_mc_crypto_dm` | `mc_crypto_dm.c`, `ed25519.c`, `ed25519_mpi.c` | DM ECDH round-trip, wrong-recipient reject, tamper reject |
| `test_integration_message` | `mc_crypto.c`, `packet.c`, `grp_txt.c` | full GRP_TXT lifecycle: frame -> encrypt -> serialize -> deserialize -> decrypt |
| `test_meshcore_packet` | `packet.c` | packet header bit-packing, transport-code gating, path length encoding, error paths |
| `test_meshcore_payloads` | `grp_txt.c`, `advert.c` | the two payload body codecs |
| `test_region_limits` / `test_region_lookup` | `region_limits.c` | sub-band boundaries, ERP/EIRP power, duty-cycle budget, country lookup |
| `test_gps_nmea` / `test_gps_parser_edge` | `gps_parser.c` | NMEA checksum, DDMM conversion, hemisphere sign, edge cases |
| `test_companion_protocol` | companion parser TUs | the P4<->C6 command parser across full/chunked/byte-by-byte feeds |

## What the gates prove (and do not)

| Gate | Proves | Does not prove |
|---|---|---|
| `make test` | pure logic + crypto vectors are correct | radio, display, C6 runtime behaviour |
| lint scripts | layering, file placement, test wiring, no dead first-party code | logic correctness |
| `make build DEVICE=tanmatsu` | the firmware compiles and links | it runs correctly on the badge |
| `identity_init` boot self-test | the shipped Ed25519 signs correctly at runtime | anything else |
| hardware smoke | the feature works on a real badge | nothing automatically; write down what you tested |

The host suite cannot exercise the radio, the display, FreeRTOS task
interleavings, or the C6 link. A concurrency fix (a mutex) compiles and the
logic tests still pass, but only hardware (or careful reasoning) confirms the
race is gone. Say so honestly in the report.

## Adding a test

1. Write `tests/test_<name>.c`. Include the real headers; the SPDX header goes
   at the top.
2. Wire it into `tests/Makefile` in all of: a `SRC_<NAME>` variable (if it needs
   extra sources), a build rule, the `all:` list, the `test:` list (with a
   `./test_<name>` run line), and `clean:`. `tests/lint/check-test-wiring.sh`
   fails the build if any of these is missing.
3. Match the build flags of a sibling rule. Common ones: `-D_GNU_SOURCE` when a
   TU uses `strnlen` (advert.c), `-Wno-type-limits -Wno-sign-compare` for the
   upstream mirror codecs, `-Istubs` + `-Wno-unused-function` when linking
   `ed25519.c`, and `$(LDLIBS)` (mbedcrypto) for anything touching mbedtls.

## Golden vectors

For crypto and wire layout, prefer a golden vector over a self-consistent
round-trip: a fixed input and a hard-coded expected output, so a regression in
either the logic or a dependency turns the test red. Generate the value once by
printing it, run the test, paste the bytes in, then switch the print to an
assert (`test_advert_sign` was built this way). Pair it with an independent
structural check (assert the byte offsets directly, not via the same helper
under test) so the test catches both a math regression and a layout regression.

## Stubs

`tests/stubs/` holds the minimal shims a shipping TU needs on the host that the
IDF would otherwise provide. `esp_random.h` is the one in use, needed to link
`ed25519.c` (keypair generation). Add a stub only when a pure-enough TU pulls a
single small platform symbol; if a TU needs many platform headers it is not
host-testable and the logic should be extracted.

## The runtime backstop

`identity_init` re-runs the RFC 8032 TV1 keypair + sign at boot and `abort()`s
on mismatch. CI catches a signer regression first, but this is the on-device
backstop: a broken crypto build refuses to start rather than transmitting
silently-rejected packets.
