# Architecture proposal: components, ports, and a testable core

Status: proposal (branch `refactor-Ilias`). Target reader: the maintainer
deciding whether to take this direction.

## Why touch a working architecture

The current layering (see [ARCHITECTURE.md](ARCHITECTURE.md)) is sound for
the domain, but it is enforced by **convention**: the six layers live in a doc
and a grep lint, and every first-party file sits flat in `main/`. That has
three concrete costs:

1. **Layer rules are not compiler-enforced.** A `render_*.c` that includes
   `meshcore/` is caught only by a grep over an enumerated file list, after the
   fact, in CI.
2. **The domain layer has no host test coverage.** Pure logic (node table
   updates, channel secret derivation, chat sanitisation, map math) is trapped
   in translation units that pull `esp_log` / `nvs` / FreeRTOS, so it cannot be
   exercised on the host. Only the fully-pure modules (codecs, region limits,
   gps parser, companion parser) are tested.
3. **Concurrency is by-convention.** Roughly eight ad-hoc mutexes guard
   per-module global tables, with at least one undocumented coupling (the
   contacts table is protected by `node_mutex`). Lock ordering is implicit.

The goal is to move from "correct by convention" to "correct by construction"
without importing web-app patterns (service/repository layers) that do not
belong on a memory-constrained MCU.

## Target structure: ESP-IDF components

Turn the logical layers into real ESP-IDF components. The component
`REQUIRES` graph is enforced by the build, so a forbidden include fails to
compile rather than tripping a lint.

```
components/
  mc_proto/    REQUIRES: -                       packet, payload/*, region_limits,
                                                  gps_parser, companion command parser  (pure, host-testable)
  mc_ports/    REQUIRES: -                       radio_port.h, store_port.h, gps_source.h  (interface headers only)
  mc_domain/   REQUIRES: mc_proto, mc_ports      nodes, chat, channels, contacts, identity, history
  mc_radio/    REQUIRES: mc_proto, mc_domain, mc_ports   radio, radio_system_protocol_client
  mc_io/       REQUIRES: mc_domain               gps*, ble_companion, companion_transport,
                                                  http_server, wifi_keepalive, sounds, cert_gen, map
  mc_ui/       REQUIRES: mc_domain               input, render, render_*, emoji
  vendor/      REQUIRES: -                       lodepng, qrcodegen, ed25519*, emoji_bitmaps
main/
  main.c          app_main + boot sequence only
  port_impl.c     device implementations wired into the ports (NVS, SD, C6 radio)
```

`main/` shrinks from ~90 files to the wiring layer. Each component carries its
own `CMakeLists.txt` with `idf_component_register(SRCS … INCLUDE_DIRS … REQUIRES …)`.

## Ports: the testability seam

Three narrow interfaces (a C struct of function pointers each), so the domain
and protocol depend on behaviour, not on the ESP-IDF hardware APIs:

- `radio_port_t`  { send, recv, get_config, get_rssi }
- `store_port_t`  { load_blob, save_blob }   (device = NVS/SD, host = in-memory)
- `gps_source_t`  { read_fix }

With fakes behind these, an end-to-end path like "advert received → node table
updated → unread badge set" becomes a host unit test. Today that path cannot be
tested off-device at all.

## Concurrency

Replace the ad-hoc per-module mutexes with one of:

- a single `domain` owner with a documented one-lock-per-table model and a
  fixed lock order, or
- an `esp_event` loop: the RX task posts typed events (`ADVERT_RX`, `DM_RX`),
  domain handlers run on one loop task and own state with no cross-task locks,
  and the UI subscribes to a `STATE_CHANGED` event to repaint.

The event-loop option removes lock-ordering and the contacts/`node_mutex`
coupling as a class of bug, at the cost of a small per-event overhead.

## Migration (incremental, each step independently buildable)

1. **Carve `mc_proto`.** Pure files, already host-tested. No behaviour change.
   **DONE** (codecs + region_limits + gps_parser + companion parser). IDF build green.
2. **Add `mc_ports` + device impls; move the domain behind the ports.** Unlocks
   host tests for nodes/chat/channels/contacts. **NOT DONE — see below.**
3. **Split `mc_radio`, `mc_io`, `mc_ui` into components; declare `REQUIRES`.**
   Layer rules become compiler-enforced; retire the grep lint. **PARTIAL:** the
   `vendor` component is extracted and the `nodes.h → radio.h` cycle is broken;
   the domain/radio/io/ui split is blocked on step 2 (below).
4. **(Optional) introduce `esp_event`** for the RX → domain → UI path. NOT DONE.

## What is done so far (branch `refactor-Ilias`)

`mc_proto` (pure core) and `vendor` (third-party) are extracted into components
with no dependency on the rest of the tree, so the build enforces those edges.
The redundant `nodes.h → radio.h` include is gone, breaking the domain→radio
header cycle. Host tests cover the protocol codecs, region limits, the GPS and
companion parsers; `tests/check-arch-rules.sh` enforces the in-`main` layering.
Every step is verified against a real `espressif/idf:v5.5.1` build.

## Why the domain/radio/io/ui split needs a deliberate, hardware-validated step

The remaining modules are not a clean stack; they are one strongly-connected
cluster. The hard edge is `radio.c` ↔ the domain: `radio.c`'s RX path does not
just hand packets to the domain, it **performs** the domain work inline — it
decrypts DMs with the identity keys, matches the channel hash, writes into the
chat rings and handles ACKs (see the `update_node` / `chat_add_*` /
`channels_find_by_hash` / `chat_mark_ack_by_crc` call sites). `settings_nvs`
adds a second knot: its header is clean, but `settings_nvs.c` reaches up into
`radio.h` (`lora_handle`), `gps_task.h` and `map.h` to push loaded config, while
`radio.c` reads the settings globals.

Splitting these into acyclic components therefore means **relocating the RX
message-processing logic out of `radio.c` into the domain** (radio delivers raw
frames through a sink/port; the domain decrypts/matches/stores) and **inverting
the settings-apply direction** (a settings-changed callback rather than
`settings_nvs.c` calling into radio/gps/map). That is a real redesign of
security-critical code, not a mechanical move. It compiles-or-not in the build
container, but it changes runtime behaviour on the radio and decrypt paths,
which only a flashed badge can confirm. It is the right next step, to be done
with hardware in the loop, not blind.

## Trade-offs

- Real churn: ~90 files relocate and gain per-component CMake. The diff is
  large even though most steps are mechanical.
- `esp_event` adds a small per-event cost; only step 4 incurs it, and only if
  the locking model is judged worth replacing.
- Build verification: the firmware builds under the ESP-IDF toolchain (CI uses
  `espressif/idf:v5.5.1`). The host test suite covers the pure and (after step
  2) the domain logic. Each migration step should be confirmed against a real
  IDF build before merge.

## Execution status (branch refactor-Ilias, supersedes the "hardware-validated redesign" caveat above)

A key finding during execution: the dependency **cycles are gone without the
risky RX-sink redesign**. The cycle was only the *backward* edges domain→radio
and domain→ui. Those are now all removed:

- `nodes.h → radio.h` (redundant include) — removed.
- `settings_nvs → radio.h / gps_task.h / map.h` — inverted (Break B): the
  C6 reconcile moved to radio.c, the shared gps/map enums to config_types.h.
- `chat → emoji.h` (pax) — broken by splitting the pax-free emoji_table out.

The remaining `radio → domain` edge is **higher→lower and legal**, so a
component split is acyclic with `mc_radio` simply REQUIRE-ing `mc_domain`. The
RX-sink decoupling ("Break A") that would make radio fully domain-independent is
**not required** for the split; it stays a documented optional purity step
(its full design is captured in the session notes) precisely because relocating
the inline decrypt/ACK logic is the one change that genuinely needs the badge.

Done and IDF-build-verified (each its own commit): `mc_proto`, `vendor`,
`mc_common` components; the three cycle-breaks above; host tests + a cppcheck
gate + the arch-rules lint.

Remaining (pure mechanical file-moves + CMake `REQUIRES`, no behaviour change,
each independently buildable) — carve in this order, building between each:
`mc_io` (nvs_helpers, gps, cert_gen) → `mc_domain` (identity, contacts,
channels, chat, nodes, history, settings_nvs; public REQUIRES tanmatsu-lora
because nodes.h/settings_nvs.h expose lora_* types) → `mc_radio` (radio,
radio_system_protocol_client; REQUIRES mc_domain) → `mc_net` (http_server, ble,
companion_transport, wifi_keepalive, gps_task, map, sounds) → `mc_ui` (input,
render*, emoji) → `main` (main.c only). A 6th component `mc_net` is needed for
acyclicity (gps_task/map/sounds read settings, so they sit above domain).
