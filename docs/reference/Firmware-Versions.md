# Firmware versions — what v3.8.0 runs on

The MeshCore app runs on top of three pieces of Nicolai-Electronics
firmware. Every radio and lora delta is upstream, and the launcher fork is
retired, so a stock install is all you need. The app pins upstream registry
versions of `tanmatsu-lora`, `tanmatsu-wifi` and `badge-bsp`.

**v3.8.0** (2026-11-08) was tested on a Tanmatsu with launcher **v0.3.1** and
`tanmatsu-radio` **v3.4.0**. It is built for BSP 2 on ESP-IDF 6.0.2, so it is
offered on boards with the second display panel; on the original panel it
runs as before. It was not tested on the second panel.

The on-device **Settings → Identity** category shows the live radio
firmware version via the system-protocol `get_information` query
introduced in `tanmatsu-radio` v3.1.1.

## 1. `tanmatsu-radio` — C6 LoRa-radio firmware

| | Version |
|---|---|
| Nicolai (upstream), tested with v3.8.0 | **v3.4.0** (2026-07-19) |
| CJ fork | retired, zero delta vs. upstream |

**Standard install:** stock upstream, as shipped by the Launcher's
Tools → Firmware update path. No fork required.

## 2. `tanmatsu-lora` — LoRa component (used by both C6 and P4)

| | Version |
|---|---|
| Nicolai (upstream), pinned by the app | **v0.6.0** (2026-07-29), `^0.6.0` in `main/idf_component.yml` |
| CJ fork | retired |

`rx_boost` and the forced `low_data_rate_optimization` have been upstream
since v0.3.0. The app's `lora_get_config` client accepts both 24-byte
(v0.2.1) and 25-byte (v0.3.0+) responses, so a build still works against an
older radio bundle (for example right after recovery, before Tools →
Firmware update has run).

## 3. `tanmatsu-launcher` — P4 launcher

| | Version |
|---|---|
| Nicolai (upstream), tested with v3.8.0 | **v0.3.1** (2026-11-01) |
| CJ fork | retired, clean upstream mirror |

**Standard install:** stock upstream from Tools → Firmware update. The three
former fork patches are gone: WiFi auto connect on boot is upstream (PR
[#141](https://github.com/Nicolai-Electronics/tanmatsu-launcher/pull/141)),
the radio version prefix match became obsolete when upstream widened the
check, and the `[C]` tag change was dropped.

## 4. `meshcore` (this app)

Not a Nicolai fork — entirely ours. Consumes upstream
`nicolaielectronics/tanmatsu-lora ^0.6.0` directly from the IDF
component registry. Current release: **v3.8.0** (2026-11-08) — see the
[changelog](../CHANGELOG.md) for the highlights.

## Recovery vs. OTA — why "Tools → Firmware update" is required after recovery

`recovery.tanmatsu.cloud` and `ota.tanmatsu.cloud` are **two different
artifact sources**. Recovery is the WebSerial flasher used to get a
bricked or blank device back on its feet; OTA is what the running
launcher pulls when you open Tools → Firmware update.

| Source | Radio | Launcher |
|---|---|---|
| `recovery.tanmatsu.cloud` (WebSerial) | `ESP-HOSTED 2.1.0` (as last checked, 2026-06) | v0.1.2 (as last checked) |
| `ota.tanmatsu.cloud` (Launcher Tools) | current upstream release | current upstream release |

Recovery lagged the OTA path by several minor releases when last checked. The
`ESP-HOSTED 2.1.0` radio firmware predates the `tanmatsu-radio` rename
(so it's pre-v3.0.0); its `lora_protocol_config_params_t` struct is
smaller than the 16-byte form the MeshCore app expects, so
`lora_get_config` returns a response shorter than either of the
fallback lengths (24 or 25 bytes). The app surfaces this as **"LoRa
radio not available"**.

**The fix is always: after recovery, open Launcher → Tools → Firmware
update before installing MeshCore.** That pulls the OTA bundle, which
brings both pieces to the current upstream releases, and MeshCore connects to the C6
on the next boot.

## What used to be ours but is now upstream (convergence)

- **RSSI/SNR + `GET_RSSI_INST`** → merged (radio PR #14 / lora PR #3),
  upstream since tanmatsu-lora v0.2.1 / tanmatsu-radio v3.1.0.
- **RX-forward fix + esp-hosted callback limit** (issue #18) → merged in
  radio **v3.1.1** (`0ca17e3` + `MAX_CUSTOM_MSG_HANDLERS=6`).
- **System-protocol `get_information`** (firmware-name / version query)
  → built upstream in `tanmatsu-radio` v3.1.1; surfaces the radio
  version on the Information tab.
- **`rx_boost` RX-sensitivity toggle** → merged in `tanmatsu-lora`
  v0.3.0 (`56671b79`, CJ's PR #5, 2026-06-03).
- **`tanmatsu-radio` v3.2.0** picks up lora v0.3.0 → the meshcore app's
  `idf_component.yml` switched back to upstream
  `nicolaielectronics/tanmatsu-lora ^0.3.0` on 2026-06-04, retiring the
  CJ forks.
- **BSP 0.9.9 display types** (launcher) → fixed upstream in `7c86493`,
  so the earlier type-mismatch patch was dropped.
- **WiFi auto connect on boot** (launcher) → merged upstream via PR #141.

**Net:** every firmware-side delta is now upstream, and the launcher fork is
retired.
