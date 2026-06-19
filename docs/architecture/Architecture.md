# Architecture (rules)

One-page discipline doc: the enforceable rules. For the design rationale and the
"how to program in this project" model, see [`Blueprint.md`](Blueprint.md). For
the descriptive module overview, FreeRTOS tasks, cold-start sequence and render
split, see [`Overview.md`](Overview.md). AI contributors: the same model as a
working handbook lives in [`.claude/`](../../.claude/Guidelines.md).

## Layers

The codebase has logical layers, now realised as ESP-IDF **components** under
`components/`. Higher layers may include lower; lower must never include higher.
The direction is enforced at build time by each component's `REQUIRES` graph, not
just by convention — a backward include fails to compile.

| # | Layer | Component | Role |
|---|---|---|---|
| L0 | Foundation | `mc_common` | App constants, view enum, global UI state, shared config enums, pax-free emoji table |
| L1 | Platform I/O | `mc_io` | NVS helpers, GPS reader, device-cert generation |
| L1 | Data state | `mc_domain` | `nodes`, `chat`, `channels`, `contacts`, `identity`, `history`, `settings_nvs`, `sounds` — in-memory tables + NVS config, mutex-protected |
| L2 | Wire protocol | `mc_proto` | MeshCore packet/payload codecs, region limits, GPS/companion parsers — mirror of upstream, host-tested |
| L2 | Channel crypto | `mc_crypto` | GRP_TXT decrypt/encrypt + the ACK-binding CRC, mbedtls only, host-tested |
| L3 | Transport | `mc_radio` | `radio.*`, `radio_system_protocol_client.*` — LoRa send/receive primitives, duty-cycle, region scope, config. Domain-free; the RX+TX handlers live in `mc_rx`. |
| L4 | Connectivity | `mc_net` | HTTP server, BLE, companion transport, WiFi keepalive, GPS task, map |
| L4 | UI | `mc_ui` | `input.*`, `render*.c`, `emoji.*` — user-facing |
| L5 | MeshCore app | `mc_rx` | RX handlers (behind the radio sink) + TX composers (called by the UI): decrypt/encrypt, domain writes, notifications, ADVERT signing, ACK |
| L5 | App entry | `main` | `main.c` — `app_main()`, boot sequence, event loop |
| — | Third-party | `vendor` | lodepng, qrcodegen, ed25519, emoji_bitmaps — kept verbatim |

## Forbidden includes

These rules are TRUE today. The point is to keep them true. They are
grep-checkable and enforced in CI by `tests/lint/check-arch-rules.sh`.

1. **`render_*.c` must not include `meshcore/*`** — UI doesn't speak the
   wire protocol; it reads and writes state through L1 (`chat`, `nodes`,
   `channels`, `contacts`).
2. **`meshcore/` must not include `pax_*`, `bsp/*`, or any L1 data
   header** — protocol code stays pure and can be reasoned about (or
   unit-tested, or moved) without dragging in the UI stack.
3. **L0–L3 files must not include `render*.h` or `input.h`** — data and
   protocol don't drive UI; UI subscribes to state.

Manual check (output should be empty; `tests/lint/check-arch-rules.sh` runs exactly
these):

```sh
grep -rE '^#include "meshcore/' components/mc_ui/render_*.c
grep -rE '^#include "(pax_|bsp/|chat|nodes|channels|contacts|settings_nvs|render)' components/mc_proto/meshcore/
grep -rE '^#include "(render|input)\.h"' \
  components/mc_proto/meshcore/ components/mc_proto/region_limits.c \
  components/mc_radio/radio*.c \
  components/mc_domain/settings_nvs.c components/mc_domain/identity.c \
  components/mc_domain/history.c components/mc_domain/chat.c \
  components/mc_domain/nodes.c components/mc_domain/contacts.c \
  components/mc_domain/channels.c
```

## Structure rules

File placement matters as much as include direction. `tests/lint/check-structure.sh`
enforces these in CI:

1. **`main/` is a thin entry point** — only `main.c` plus the build files
   (`CMakeLists.txt`, `idf_component.yml`). New first-party code lives in a
   component, never back in `main/`. This is the rule that keeps the tree from
   collapsing into one fat module again.
2. **Every component registers itself** with a `CMakeLists.txt` declaring its
   `SRCS`, `INCLUDE_DIRS` and `REQUIRES`/`PRIV_REQUIRES`.
3. **First-party C source carries an SPDX header** (`SPDX-FileCopyrightText`
   + `SPDX-License-Identifier: MIT`). Vendored libraries and the upstream
   companion parser keep their own upstream notices and are exempt.
4. **The repository root stays on an allowlist** — docs go in `docs/`, scripts
   in `scripts/`, source in a component. A new top-level file is rejected until
   it is added to the allowlist on purpose.

## Developing within the architecture

The layers above are real components; the build refuses a backward include, so
the discipline is mechanical, not just a promise. A full per-component tour and
the RX/TX data flow live in [`Components.md`](Components.md); the short
version for keeping the tree tidy:

**Where does new code go?**

- Pure, host-testable protocol/codec logic → `mc_proto` (no ESP-IDF) or
  `mc_crypto` (mbedtls only). Add a host test in `tests/`.
- New persisted state or app logic → `mc_domain`.
- New platform I/O wrapper (NVS, a sensor, a peripheral) → `mc_io`.
- New radio/transport behaviour → `mc_radio`.
- New connectivity or off-radio peripheral (HTTP, BLE, GPS task, map) → `mc_net`.
- New screen, input handling or rendering → `mc_ui`.
- App wiring / boot order only → `main.c`. Nothing else belongs in `main/`.

**Dependency direction** (higher REQUIRES lower; never the reverse):

```
main
 └─ mc_ui, mc_net, mc_rx
     └─ mc_radio ─ mc_crypto
         └─ mc_domain
             └─ mc_io
                 └─ mc_common, mc_proto, vendor   (leaves)
```

`mc_rx` is the MeshCore application layer: the radio transport deserializes +
dedups a received packet and hands it to a registered sink (`radio_set_rx_sink`)
that `mc_rx` implements; the UI calls `mc_rx`'s TX composers to send. Both the RX
decrypt/delivery and the TX composition live in `mc_rx`, so `mc_radio` is pure
transport — it builds no MeshCore payloads and touches no domain message state
(it only reads LoRa/region config).

If a new include would point "up" this list, that is a design smell: either the
code sits in the wrong component, or the shared type belongs in a lower one
(usually `mc_common` for enums/state, `mc_proto` for wire types).

**Adding a new component:**

1. `mkdir components/mc_<name>` and write its `CMakeLists.txt` — `SRCS`,
   `INCLUDE_DIRS "."`, and only the `REQUIRES` it needs (public ones for types
   exposed in its headers, `PRIV_REQUIRES` for the rest).
2. Move the sources in with `git mv`; drop them from the old component's `SRCS`.
3. Add the new component to its dependents' `REQUIRES`.
4. `make build DEVICE=tanmatsu`, then run the lints and host tests. For a pure
   move the binary size should be unchanged.

Keep components cohesive and few — read "When to add a new layer" below before
reaching for a new one.

## Wire-boundary discipline (Nicolai/upstream compat)

Three files cross the firmware boundary. Bugs here have cost the most
real-user pain in this project (`rx_boost` ABI break, region-scope `#`
HMAC prefix). They get the strictest rules.

| File | Tracks upstream |
|---|---|
| `components/mc_proto/meshcore/packet.{c,h}` + `meshcore/payload/*` | MeshCore protocol — Scott Powell / rippleradios.com |
| `components/mc_radio/radio_system_protocol_client.{c,h}` | Tanmatsu radio system-protocol — Nicolai-Electronics/tanmatsu-radio |
| `components/mc_radio/radio.c` (region-scope HMAC) + `components/mc_rx/mc_rx.c` (payload composition, ADVERT signing, DM/channel framing) | MeshCore wire-format with our region-scope additions |

Rules at this boundary:

1. **Every wire-boundary file carries an `upstream:` comment** at the
   top, naming the upstream repo and the reference commit/version. Bump
   on every sync. Future-you reading this code in six months should know
   instantly which upstream commit defines the byte layout.
2. **Tolerant parsers, never strict equality on size** — accept any
   forward-compatible struct length by reading what's there and
   defaulting absent fields. The `lora_get_config` consumer pattern
   accepts both 24-byte (pre-`rx_boost`) and 25-byte (post-`rx_boost`)
   responses for this reason.
3. **Never extend a wire-format struct locally** — open the PR upstream
   first and pin the dependency once it lands. Local growth of a
   protocol struct is a breaking change for every other consumer (see
   devlog EN lesson 61).
4. **Region-scope and other local extensions live in `mc_radio`, not in
   `mc_proto/meshcore/`** — `meshcore/` is the upstream mirror and stays
   pure; the symmetric crypto lifted out of `radio.c` lives in `mc_crypto`,
   and the RX handlers (decrypt + delivery + ACK) in `mc_rx`.
5. **Pure layout descriptors may live in `mc_proto` root** (alongside
   `region_limits.c`), never under `meshcore/`. `mc_proto/advert_sign.c`
   defines the ADVERT signable-byte range as a pure function so the layout
   is host-tested (`test_advert_sign`) independently of the ed25519 math;
   `mc_rx` does the actual signing. Splitting "which bytes" (testable) from
   "sign them" (needs the key + RNG) keeps both halves gated in CI.

## When to add a new layer

Don't. Adding a layer is worth it only when the same operation already
repeats across 3+ files and pulling it out makes the result shorter, not
longer. Three similar lines beat premature abstraction. If the words
`network_layer.h`, `repository.h`, `service.h` come to mind, you're
probably over-engineering for a ~50-file C app.

## Unused code

First-party code carries no dead weight: a function with no caller anywhere
in `components/`, `main/`, or `tests/` is removed together with its
declaration, not left "for later". `cppcheck --enable=unusedFunction`
(run by `tests/lint/check-cppcheck.sh`) is the starting signal, but it is
advisory only — it reports several live symbols as unused and must be
cross-checked by grep before anything is deleted. Four categories it flags
are deliberately kept:

- **Vendored libraries** (`components/vendor/` — lodepng, qrcodegen,
  ed25519). Complete third-party drops; unused entry points stay so the
  files match upstream. See rule 1 in `CLAUDE.md`.
- **The upstream protocol mirror** (`mc_proto/companion-radio-protocol/`).
  Kept byte-for-byte in sync with upstream; not edited locally.
- **`app_main`** — the ESP-IDF entry point, called by the framework, so it
  has no in-tree caller.
- **Function-pointer callbacks and test-only symbols.** The Settings UI
  installs `save_*` handlers into a field-dispatch table by address
  (`render_settings.c`), and helpers such as `region_effective_power_dbm`
  are exercised only from `tests/`. cppcheck counts neither as a call.

Everything else with no caller is gone.

## See also

- [`Overview.md`](Overview.md) — descriptive
  module overview, FreeRTOS tasks, cold-start sequence, render split
- [`Settings-NVS.md`](../reference/Settings-NVS.md) — NVS key
  scheme and migration history
- Memory references (CLAUDE/CJ session-only):
  `tanmatsu-meshcore-workflow` (branch protection + PR flow),
  `tanmatsu-meshcore-ci` (CI setup)
