# Architecture proposal: components, ports, and a testable core

Status: proposal (branch `refactor-Ilias`). Target reader: the maintainer
deciding whether to take this direction.

## Why touch a working architecture

The current layering (see [ARCHITECTURE.md](../ARCHITECTURE.md)) is sound for
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
2. **Add `mc_ports` + device impls; move the domain behind the ports.** Unlocks
   host tests for nodes/chat/channels/contacts.
3. **Split `mc_radio`, `mc_io`, `mc_ui` into components; declare `REQUIRES`.**
   Layer rules become compiler-enforced; retire the grep lint.
4. **(Optional) introduce `esp_event`** for the RX → domain → UI path.

## Trade-offs

- Real churn: ~90 files relocate and gain per-component CMake. The diff is
  large even though most steps are mechanical.
- `esp_event` adds a small per-event cost; only step 4 incurs it, and only if
  the locking model is judged worth replacing.
- Build verification: the firmware builds under the ESP-IDF toolchain (CI uses
  `espressif/idf:v5.5.1`). The host test suite covers the pure and (after step
  2) the domain logic. Each migration step should be confirmed against a real
  IDF build before merge.
