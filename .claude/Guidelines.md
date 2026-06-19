# Working on this project with Claude

Rules and context for an AI pair programmer (or any contributor using one) on
MeshCore for the Tanmatsu badge. This is the entry point for the `.claude/`
guidance set. Read the one that matches what you are about to do:

- **Guidelines.md** (this file): the mental model, where code goes, the hard
  rules, conventions, and the green gate.
- **[Components.md](Components.md)**: per-component reference, the dependency
  graph, the constants, and which file owns what.
- **[Data-Flows.md](Data-Flows.md)**: the cold-start order and the RX / TX /
  advert / DM / channel flows with real function names. Read before chasing a
  symptom to the wrong file.
- **[Crypto.md](Crypto.md)**: the signing, channel, DM, region-scope and ACK
  crypto, the ed25519 split, and the gates. Read before touching anything that
  signs, encrypts, or derives a key.
- **[Testing.md](Testing.md)**: the host test harness, what each gate proves,
  golden vectors, and how to add a test.
- **[Build-And-CI.md](Build-And-CI.md)**: build invocation, board targets, and
  the two deliberately-divergent CI workflows.
- **[Workflow.md](Workflow.md)**: how to carry a change from first read to a
  green commit, including how to verify the firmware build when you lack the
  IDF toolchain.
- **[Pitfalls.md](Pitfalls.md)**: the traps that have already cost real time or
  real users here. Read it before you trust a tool or an assumption.

Read these together with the root [CLAUDE.md](../CLAUDE.md),
[docs/Blueprint.md](../docs/Blueprint.md) (the design rationale and how to
program here), [docs/Architecture.md](../docs/Architecture.md) (the rulebook) and
[docs/Components.md](../docs/Components.md) (the component and data flow tour).
This `.claude/` set is the same model as the Blueprint, written as working rules.
When this set and Architecture.md ever disagree, Architecture.md wins and you
should fix this file.

## What this project is

A MeshCore LoRa mesh chat client for the Tanmatsu badge: an ESP32-P4 app
processor talking to an ESP32-C6 radio coprocessor over a system protocol, with
an SX1262 doing the actual LoRa. One ESP-IDF v5.5.1 image, C11. Concurrency is a
handful of FreeRTOS tasks plus the `app_main` event loop, not threads with
shared heaps everywhere. Most state lives behind a mutex or inside one task.

## The mental model

The code is split into ESP-IDF **components** under `components/`, layered L0
(foundation) to L5 (app entry). The layering is not a convention you remember,
it is enforced by each component's `REQUIRES` / `PRIV_REQUIRES` graph: a
backward include fails to **compile**, so you find out at build time, not in
review. Before writing a line, know which component your change belongs in and
which layer it sits on. Architecture.md "Layers" and "Forbidden includes" are
the authority.

Two consequences worth internalising:

- A change often does not go where the symptom is. A UI glitch may be a domain
  write under the wrong lock, a dropped advert may be a transport or a framing
  issue. Trace the data flow (Components.md) before picking a file.
- If your fix wants a lower layer to include a higher one, the design is telling
  you the code is in the wrong place, not that the rule is wrong. Move the code,
  add a callback, or pass data down. Do not reach for a backward include.

## Where code goes

| Kind of code | Component | Notes |
|---|---|---|
| Pure protocol / codec / regulatory / parser logic | `mc_proto` | No ESP-IDF, pax or BSP. Host testable. Holds the upstream mirror under `meshcore/` plus first-party pure helpers in the root. |
| Symmetric channel crypto | `mc_crypto` | Host tested. Add a vector for anything you touch. |
| DM crypto (ed25519 ECDH) | `mc_crypto` (`mc_crypto_dm.c`) | A file in the `mc_crypto` component, not a separate component; split so the channel tests stay ed25519 free. |
| Persisted state and app data | `mc_domain` | Settings, nodes, contacts, chat, channels, identity. |
| Platform I/O wrappers | `mc_io` | NVS, GPS, certs, SD. Thin shims over ESP-IDF. |
| LoRa transport | `mc_radio` | Send and receive primitives, duty cycle, region scope. **Domain free**: it builds no MeshCore payloads. |
| MeshCore receive handlers and TX composers | `mc_rx` | Owns decrypt, encrypt, domain writes, notifications, ACK. Radio hands it raw packets via the RX sink and `radio_tx_message`. |
| Screens, input, rendering | `mc_ui` | The `render_*.c` files. Must not include `meshcore/`. |
| Connectivity and peripherals | `mc_net` | HTTP server, maps, WiFi glue. |
| Third party drops and generated assets | `vendor` | See hard rules. Leaf component, never imports first-party code. |

`main/` is `main.c` only: the cold start sequence and the event loop. New
first-party code goes in a component, never back into `main/`.
`tests/lint/check-structure.sh` enforces both this and a clean repo root.

## Hard rules (these have bitten real users)

- **Do not modify vendored code.** `components/vendor/*` (lodepng, qrcodegen,
  ed25519, emoji_bitmaps) are third party drops kept close to upstream. Their
  TODO markers are upstream comments, not work items. If a vendored function
  looks unused, it usually is, and it stays anyway so the file matches upstream.
  See [Pitfalls.md](Pitfalls.md) on dead code and on the ed25519 split.
- **`components/mc_proto/meshcore/` is the upstream protocol mirror.** Keep it
  free of ESP-IDF, pax, BSP and L1 headers. Never grow a wire format struct
  locally: take the change upstream and re-pin the dependency. Local additions
  live in `mc_radio` (region scope) or `mc_rx` (framing), or as a pure
  first-party helper in `mc_proto` root, never inside `meshcore/`.
- **The wire boundary is fragile.** `radio.c` (region scope HMAC), `mc_rx`
  (payload composition, ADVERT signing, DM and channel framing) and
  `radio_system_protocol_client.c` track upstream byte for byte. Use tolerant
  parsers and never assume an exact struct size. Architecture.md "Wire-boundary
  discipline" lists the offsets and the past bugs.
- **Crypto correctness is gated twice and you keep it that way.** Signing math
  is proven by host vectors (`test_ed25519`, RFC 8032) and by a runtime boot
  self-test in `identity_init()` that calls `abort()` on mismatch. The signed
  byte **layout** is proven by `test_advert_sign`. If you touch signing, both
  gates must still pass and a new layout needs a new vector. The original
  direct-advert bug hid for months because a sender never sees rejections.
- **Behaviour that only shows on the badge is validated on a badge.** The host
  tests and the IDF build prove that code compiles and that pure logic is
  correct. They do not run the radio, the display or the C6 link. Anything in
  that space needs a hardware smoke test, and you say so plainly rather than
  implying CI covered it.

## Conventions

- Every first-party C source starts with an SPDX header
  (`SPDX-FileCopyrightText: 2026 CJ van Soest` and
  `SPDX-License-Identifier: MIT`). Add an `SPDX-FileContributor` line for new
  authorship. Do not add a copyright line for a contributor and do not write
  licence waiver prose.
- Comments are sparse. Explain the non-obvious (a wire quirk, a locking
  coupling, why a workaround exists), never restate what the code says. If a
  block needs a paragraph, the paragraph probably belongs in `docs/`.
- All repo text (code, comments, commit messages, docs) is in English. Chat with
  the user can be in their language.
- Write plainly. No em dash, no coined hyphen-adjectives, no serial comma. This
  applies to comments and docs too.
- Format touched files with `.clang-format`.
- Commit messages: imperative subject, short body, at most a few lines, no AI
  attribution trailer and no co-author line.

## Dead code policy

First-party code carries no dead weight. A function with no caller anywhere in
`components/`, `main/` or `tests/` is removed together with its declaration.
`cppcheck --enable=unusedFunction` is the starting signal only and it reports
several live symbols as unused: cross-check with grep first. The deliberate
keeps (vendored libraries, the upstream mirror, `app_main`, callback tables,
test-only symbols) are documented in Architecture.md "Unused code" and the
mechanics of the false positives are in [Pitfalls.md](Pitfalls.md).

## Before you commit: all green

```sh
cd tests && make test                  # host unit + integration tests + crypto vectors
tests/lint/check-arch-rules.sh         # include direction discipline
tests/lint/check-structure.sh          # file placement (main/ thin, root clean)
tests/lint/check-test-wiring.sh        # every test_*.c is wired into the Makefile
tests/lint/check-cppcheck.sh           # static analysis, first-party only
make build DEVICE=tanmatsu             # the firmware actually builds
```

Add a host test in `tests/` for any pure logic you add or change and wire it
into `tests/Makefile`. The wiring lint fails the build if you forget. The full
change loop, including how to run the firmware build when you do not have the
IDF toolchain locally, is in [Workflow.md](Workflow.md).
