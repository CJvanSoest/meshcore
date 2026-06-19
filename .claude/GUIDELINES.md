# Working on this project with Claude

Rules and context for an AI pair-programmer (or any contributor using one) on
MeshCore for Tanmatsu. Read this together with the root [CLAUDE.md](../CLAUDE.md),
[docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md) (the rulebook) and
[docs/Components.md](../docs/Components.md) (the component + data-flow tour)
before changing code.

## Before you touch anything

1. The codebase is split into ESP-IDF **components** under `components/`. The
   `REQUIRES` graph enforces the layer direction — a backward include fails to
   compile, not just review. Know which component your change belongs in
   (see ARCHITECTURE.md "Developing within the architecture").
2. `main/` is `main.c` only. New first-party code goes in a component, never
   back into `main/`. `tests/lint/check-structure.sh` enforces this.

## Where code goes

- Pure, host-testable protocol/codec/regulatory logic → `mc_proto` (no ESP-IDF).
- Symmetric crypto → `mc_crypto` (channel) / `mc_crypto_dm` (DM, ed25519). Both
  are host-tested; add a vector for anything you touch.
- Persisted state / app data → `mc_domain`. Platform I/O wrappers → `mc_io`.
- LoRa transport (send/receive primitives, duty-cycle, region scope) →
  `mc_radio`. It is **domain-free**: it builds no MeshCore payloads.
- MeshCore receive handlers + TX composers → `mc_rx` (it owns decrypt/encrypt,
  domain writes, notifications, ACK; radio just hands it raw packets via the
  sink and `radio_tx_message`).
- Screens / input / rendering → `mc_ui`. Connectivity + peripherals → `mc_net`.

## Hard rules (these have bitten real users)

- **Do not modify vendored code** (`components/vendor/*`: lodepng, qrcodegen,
  ed25519, emoji_bitmaps). Their TODO markers are upstream, not work items.
- **`components/mc_proto/meshcore/` is the upstream protocol mirror.** Keep it
  free of ESP-IDF / pax / BSP / L1 headers. Never grow a wire-format struct
  locally — take it upstream and re-pin. Local additions go in `mc_radio`
  (region scope) or `mc_rx` (framing), never in `meshcore/`.
- **The wire boundary is fragile.** `radio.c` (region-scope HMAC), `mc_rx`
  (payload composition, ADVERT signing) and `radio_system_protocol_client.c`
  track upstream byte-for-byte. Use tolerant parsers; never assume an exact
  struct size. See ARCHITECTURE.md "Wire-boundary discipline".
- **Behaviour that only shows on the badge is validated on a badge.** The host
  tests and the IDF build cannot prove radio/display runtime behaviour.

## Conventions

- Every first-party C source starts with an SPDX header
  (`SPDX-FileCopyrightText` + `SPDX-License-Identifier: MIT`); add an
  `SPDX-FileContributor` line for new authorship.
- Comments are sparse — explain the non-obvious (a wire quirk, a locking
  coupling), not the obvious. Format with `.clang-format`.
- All repo text (code, comments, commits, docs) is in English.
- Commit messages: short and clear, imperative subject, no AI-attribution
  trailer.

## Before you commit — all green

```sh
cd tests && make test                  # host unit + integration tests
tests/lint/check-arch-rules.sh         # include-direction discipline
tests/lint/check-structure.sh          # file placement (main/ thin, root clean)
tests/lint/check-test-wiring.sh        # every test_*.c is wired into the Makefile
tests/lint/check-cppcheck.sh           # static analysis, first-party only
make build DEVICE=tanmatsu             # the firmware actually builds
```

Add a host test in `tests/` for any pure logic you add or change, and wire it
into `tests/Makefile` (the wiring lint will catch you if you forget).
