# Blueprint

How this project is built and how to program within it. This is the design
rationale behind the refactor and the standing model every contributor follows.
Read it once before your first change. It complements two neighbours:

- [Architecture.md](Architecture.md) is the enforceable rulebook (the layers,
  the forbidden includes, the grep checks). When a rule and this narrative
  disagree, Architecture.md wins.
- [Components.md](Components.md) is the component-by-component map.

AI contributors using Claude should also read the `.claude/` handbook (start at
`.claude/Guidelines.md`); it is the same model written as working rules.

## Why the refactor happened

The app began as a flat `main/` directory: one growing pile of `.c` files where
a UI handler could reach straight into the radio, the radio could parse a
payload, and crypto was inlined wherever it was needed. That shape made three
things hard. Nothing could be unit-tested without an ESP32 in the loop. A change
in one corner quietly broke another because everything could call everything.
And the wire-facing code (the part that must match other MeshCore nodes
byte-for-byte) was tangled up with display and storage code, so a refactor risked
breaking interop with no test to catch it.

The refactor carved that flat tree into ESP-IDF **components** with a one-way
dependency graph, pulled the pure logic out so it can be host-tested, and made
the radio a dumb transport that knows nothing about MeshCore payloads. The point
was not tidiness for its own sake. It was to make the compiler enforce the
boundaries, and to make the parts that matter most (crypto, wire format) testable
off-device.

## The model

### Compiler-enforced layers

Code lives in `components/`, layered L0 (foundation) to L5 (app entry). Each
component declares what it may use via `REQUIRES` / `PRIV_REQUIRES`. Includes
point only downward. A backward include does not just fail review, it fails to
**compile**. So the architecture is not a document you have to remember, it is a
property the build checks.

```
L0  mc_common                      shared defs
L1  mc_io           mc_domain      platform I/O ; persisted state + domain
L2  mc_proto  mc_crypto            pure protocol/codec ; symmetric + DM crypto
L3  mc_radio                       LoRa transport (domain free)
L4  mc_net    mc_ui                connectivity ; screens + input
L5  mc_rx     main                 RX handlers + TX composers ; entry point
    vendor                         third-party leaf
```

`main/` is `main.c` only. New first-party code goes in a component, never back
into `main/`. The full dependency table and constants are in
[Components.md](Components.md).

### Pure where it can be, platform only where it must be

`mc_proto` (protocol codecs, regulatory tables, NMEA, the advert signable-byte
helper) and the crypto components carry no ESP-IDF, pax or BSP dependency in
their pure parts. That is deliberate: it lets the host test suite link the
**real shipping `.c` files** and prove they are correct off-device. Do not pull
a platform header into a currently-pure translation unit without a strong
reason; you would forfeit that test coverage. When you add pure logic, it
belongs in `mc_proto` or a crypto component and it gets a host test.

### The radio is a dumb pipe

`mc_radio` is pure transport. It serializes and deserializes bytes, deduplicates
received frames, tracks the duty-cycle budget, applies the region scope, and
hands a raw `meshcore_message_t` to a sink callback. It builds no MeshCore
payload and decrypts nothing. All protocol logic (decode, decrypt, deliver, ACK,
and the TX composers) lives one layer up in `mc_rx`. The two are decoupled
through `radio_set_rx_sink(fn)` for receive and the single `radio_tx_message()`
primitive for send. This is the move that lets the protocol brain be changed
without touching the transport, and vice versa. The runtime flows are drawn in
[`.claude/Data-Flows.md`](../.claude/Data-Flows.md).

### The wire boundary is sacred

A handful of files must match upstream MeshCore (and the Tanmatsu radio
firmware) exactly: the protocol mirror under `mc_proto/meshcore/`, the
region-scope HMAC in `mc_radio`, the payload composition and signing in `mc_rx`,
and the P4 to C6 system-protocol client. Rules: never grow a wire-format struct
locally (a fix goes upstream first, then the dependency is re-pinned), keep
`mc_proto/meshcore/` a pure upstream mirror with no local edits, and use tolerant
parsers that never assume an exact struct size. The most painful bugs this
project has shipped were here. See Architecture.md "Wire-boundary discipline".

### Crypto is split and double-gated

Channel crypto lives in `mc_crypto`, DM crypto in `mc_crypto_dm` (split so the
channel tests stay free of the ed25519 dependency). The Ed25519 signer and the
X25519 ECDH path are two separate vendored translation units that divide by
symbol, not by "main vs variant". Signing correctness is gated by host vectors
and by a boot self-test that `abort()`s on mismatch; the signable byte layout is
gated separately. The full picture, including the trap that hid a broken signer
for months, is in [`.claude/Crypto.md`](../.claude/Crypto.md).

### No dead weight, and test-or-it-did-not-happen

First-party code with no caller anywhere (components, main, tests) is removed
with its declaration. Any pure logic you add or change gets a host test wired
into the build. The deliberate keeps (vendored libraries, the upstream mirror,
the entry point, callback tables, test-only symbols) are documented so they are
not mistaken for dead code. See Architecture.md "Unused code".

## How to make a change

1. Find the layer and component the change belongs in. If a fix wants a lower
   layer to include a higher one, the code is in the wrong place: move it, add a
   callback, or pass data down. Do not reach for a backward include.
2. Keep `mc_radio` domain free and `mc_proto/meshcore/` a pure mirror. New local
   behaviour goes in `mc_rx`, `mc_radio`, or a pure helper in `mc_proto` root.
3. Prefer extracting pure logic out of a non-testable function so a host test
   can reach it (the advert signable-byte helper exists for exactly this).
4. Add or update a host test and wire it into `tests/Makefile`. For crypto and
   wire layout, prefer a golden vector plus an independent structural check.
5. Run the full green gate (below) and build the firmware before you trust it.

### Adding a new feature

Put state in `mc_domain`, pure logic in `mc_proto` (with a test), platform I/O
in `mc_io`, screens in `mc_ui`, and any protocol handling in `mc_rx`. Wire the
component dependencies in its `CMakeLists.txt`. If the feature sends on the
radio, build a `meshcore_message_t` in `mc_rx` and call `radio_tx_message`; do
not reach into the radio from the UI.

### Adding a new component

Resist it unless the same operation already repeats across three or more files
and pulling it out makes the result shorter. Three similar lines beat a
premature abstraction. When you do add one, give it the lowest layer that works,
declare a minimal `REQUIRES`, and keep the graph acyclic.

### Touching the wire or crypto

Open the upstream change first if it is a protocol change. Locally, add the
behaviour in `mc_rx` / `mc_radio`, never in the mirror. Keep both crypto gates
green and add a vector for anything you touch.

## Conventions

- Every first-party source starts with an SPDX header
  (`SPDX-FileCopyrightText` + `SPDX-License-Identifier: MIT`); add an
  `SPDX-FileContributor` line for new authorship.
- Comments are sparse: explain the non-obvious (a wire quirk, a locking
  coupling), never restate the code. Long explanations belong in `docs/`.
- All repo text is English. Format touched files with `.clang-format`.
- Commit messages: imperative subject, short body, no AI-attribution trailer.

## The green gate

```sh
cd tests && make test                  # host unit + integration + crypto vectors
tests/lint/check-arch-rules.sh         # include-direction discipline
tests/lint/check-structure.sh          # file placement (main/ thin, root clean)
tests/lint/check-test-wiring.sh        # every test_*.c is wired into the Makefile
tests/lint/check-cppcheck.sh           # static analysis, first-party
make build DEVICE=tanmatsu             # the firmware actually builds
```

The host gate and the build run in CI on every push. Behaviour that only shows
on the badge (radio, display, the C6 link, task interleavings) is validated on a
badge; the host suite cannot prove it. Say so honestly when you report.

## See also

- [Architecture.md](Architecture.md) — the enforceable rules
- [Components.md](Components.md) — the component map and constants
- [Overview.md](Overview.md) — descriptive module + task overview
- `.claude/` — the same model as a working handbook for AI contributors
