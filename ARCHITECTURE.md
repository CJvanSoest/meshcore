# Architecture (rules)

One-page discipline doc. For descriptive module overview, FreeRTOS tasks,
cold-start sequence, render split — see
[`docs/wiki/Architecture.md`](docs/wiki/Architecture.md).

## Layers

The codebase has six logical layers. Higher layers may include lower; lower
must never include higher.

| # | Layer | Files | Role |
|---|---|---|---|
| L0 | Foundation | `app_config.h`, `ui_state.h` | App constants, view enums, global UI state |
| L1 | Data state | `nodes`, `chat`, `channels`, `contacts`, `identity`, `history`, `settings_nvs` | In-memory tables + NVS-backed config, mutex-protected |
| L2 | Wire protocol | `meshcore/packet.*`, `meshcore/payload/*` | MeshCore packet/payload codecs — mirror of upstream |
| L3 | Comm bridge | `radio.*`, `radio_system_protocol_client.*`, `region_limits.*` | LoRa TX/RX tasks, system-protocol queries, regulatory clamps. Composes L1+L2 with HW. |
| L4 | UI | `input.*`, `render.*`, `render_*.c`, `render_internal.h`, `qrcodegen.*`, `emoji.*` | User-facing |
| L5 | App entry | `main.c` | `app_main()`, boot sequence, event loop |

## Forbidden includes

These rules are TRUE today. The point is to keep them true. They are
grep-checkable, intended to migrate into CI when the workflow grows a
lint job.

1. **`render_*.c` must not include `meshcore/*`** — UI doesn't speak the
   wire protocol; it reads and writes state through L1 (`chat`, `nodes`,
   `channels`, `contacts`).
2. **`meshcore/` must not include `pax_*`, `bsp/*`, or any L1 data
   header** — protocol code stays pure and can be reasoned about (or
   unit-tested, or moved) without dragging in the UI stack.
3. **L0–L3 files must not include `render*.h` or `input.h`** — data and
   protocol don't drive UI; UI subscribes to state.

Manual check (output should be empty):

```sh
grep -rE '^#include "meshcore/' main/render_*.c
grep -rE '^#include "(pax_|bsp/|chat|nodes|channels|contacts|settings_nvs|render)' main/meshcore/
grep -rE '^#include "(render|input)\.h"' main/meshcore/ main/radio*.c main/region_limits.c main/settings_nvs.c main/identity.c main/history.c main/chat.c main/nodes.c main/contacts.c main/channels.c
```

## Wire-boundary discipline (Nicolai/upstream compat)

Three files cross the firmware boundary. Bugs here have cost the most
real-user pain in this project (`rx_boost` ABI break, region-scope `#`
HMAC prefix). They get the strictest rules.

| File | Tracks upstream |
|---|---|
| `meshcore/packet.{c,h}` + `meshcore/payload/*` | MeshCore protocol — Scott Powell / rippleradios.com |
| `radio_system_protocol_client.{c,h}` | Tanmatsu radio system-protocol — Nicolai-Electronics/tanmatsu-radio |
| `radio.c` (transport-codes, region-scope HMAC, ADVERT signing) | MeshCore wire-format with our region-scope additions |

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
4. **Region-scope and other local extensions live in `radio.c`, not in
   `meshcore/`** — `meshcore/` is the upstream mirror and stays pure;
   anything we add on top sits outside that directory.

## When to add a new layer

Don't. Adding a layer is worth it only when the same operation already
repeats across 3+ files and pulling it out makes the result shorter, not
longer. Three similar lines beat premature abstraction. If the words
`network_layer.h`, `repository.h`, `service.h` come to mind, you're
probably over-engineering for a ~50-file C app.

## See also

- [`docs/wiki/Architecture.md`](docs/wiki/Architecture.md) — descriptive
  module overview, FreeRTOS tasks, cold-start sequence, render split
- [`docs/wiki/Settings-NVS.md`](docs/wiki/Settings-NVS.md) — NVS key
  scheme and migration history
- Memory references (CLAUDE/CJ session-only):
  `tanmatsu-meshcore-workflow` (branch protection + PR flow),
  `tanmatsu-meshcore-ci` (CI setup)
