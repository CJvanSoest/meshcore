# Firmware versions — our forks vs. Nicolai (upstream)

The MeshCore app runs on top of three pieces of Nicolai-Electronics
firmware. As of v2.2.0 (2026-06-04) **every radio/lora delta is upstream**:
both rx_boost and `low_data_rate_optimization` shipped in
`tanmatsu-lora` v0.3.0, picked up by `tanmatsu-radio` v3.2.0, and the
MeshCore app now pins upstream registry versions instead of the CJ
forks. The launcher fork still carries three small UX patches.

The on-device **Settings → Identity** category shows the live radio
firmware version via the system-protocol `get_information` query
introduced in `tanmatsu-radio` v3.1.1.

## 1. `tanmatsu-radio` — C6 LoRa-radio firmware

| | Version | Commit |
|---|---|---|
| Nicolai (upstream) | **v3.2.0** | `d83ad84` |
| `ota.tanmatsu.cloud` (Launcher Tools) | v3.2.0 | `d83ad84` |
| `recovery.tanmatsu.cloud` | `ESP-HOSTED 2.1.0` (pre-v3.0.0 rename) | — |
| CJ fork (retired) | v3.1.1 **+1** | `f919f91` |

**Standard install:** stock upstream **v3.2.0**, as shipped by the
Launcher's Tools → Firmware update path. No fork required.

**v3.2.0** (released 2026-06-04) is a one-line dependency bump that
pulls in `tanmatsu-lora` v0.3.0 — see § 2 for what that brings.

**CJ fork status:** retired. The `f919f91` commit only redirected
`idf_component.yml` to a privately hosted `tanmatsu-lora` fork while
rx_boost was out-of-tree; with rx_boost upstream in lora v0.3.0 the
redirect is moot and the CJ/tanmatsu-radio fork has zero delta vs.
upstream main.

## 2. `tanmatsu-lora` — LoRa component (used by both C6 and P4)

| | Version | Commit |
|---|---|---|
| Nicolai (upstream) | **v0.3.0** | `bdda8be` |
| Previous upstream (used by radio v3.1.1) | v0.2.1 | `b9e87c5` |
| CJ fork (retired) | v0.2.1 + rx_boost | `db18049` |

**v0.3.0** (released 2026-06-03) contains:
- `rx_boost` config flag + RxGain register write (CJ's PR #5, merged
  2026-06-03 as `4cc38f95`)
- Force `low_data_rate_optimization` when SF/BW require it (`bdda8be`)

**App now pins upstream.** `main/idf_component.yml` references
`nicolaielectronics/tanmatsu-lora ^0.3.0` from the IDF component
registry — no more `git:` redirect to a private fork. The 17-byte
config struct is identical on both sides.

**Legacy fallback still present.** The app's `lora_get_config` client
accepts both 24-byte (v0.2.1) and 25-byte (v0.3.0+) responses, so the
same MeshCore build still works against an older radio bundle (e.g.
post-recovery before Tools → Firmware update has run).

## 3. `tanmatsu-launcher` — P4 launcher

| | Version | Commit |
|---|---|---|
| Nicolai (upstream) | **v0.1.7** | (current main) |
| CJ fork | v0.1.4 **+3** | `9dd36e8` |
| `ota.tanmatsu.cloud` (Launcher Tools) | v0.1.7 | (current main) |
| `recovery.tanmatsu.cloud` | v0.1.2 | — |

**Standard install:** stock upstream **v0.1.7** (from Tools → Firmware
update). Stock v0.1.7 widens the radio version check, so the
version-mismatch screen no longer pops on the v3.2.0 radio.

**Delta (3 commits, dev/personal):**
1. **WiFi auto-connect on boot** — upstream only connects when NTP is
   enabled; the fork connects unconditionally so apps have network
   after every reboot. **Submitted upstream**: PR
   [#141](https://github.com/Nicolai-Electronics/tanmatsu-launcher/pull/141),
   open.
2. **Accept the whole v3.1.x radio line** — upstream `a32c7ee` (in
   v0.1.6) hard-compares against `"v3.1.1"`. A git-described developer
   radio (`v3.1.1-1-g<sha>`) trips this; the fork prefix-matches
   `v3.1.` so dev builds don't pop a mismatch warning. Not submitted
   (stock users hit the exact-match path; this is dev ergonomics only).
3. **Hide the `[C]` cached tag** in the app list (the `[M]` mismatch
   tag stays). Cosmetic, not submitted.

For stock end users the launcher fork is **optional** — the only
behavioural diff vs. upstream v0.1.6 is the WiFi auto-connect on boot.

## 4. `meshcore` (this app)

Not a Nicolai fork — entirely ours. Consumes upstream
`nicolaielectronics/tanmatsu-lora ^0.3.0` directly from the IDF
component registry. Current release: **v2.2.0** (2026-06-04) — see the
[docs index](../README.md) for the v2.2.0 highlights.

## Recovery vs. OTA — why "Tools → Firmware update" is required after recovery

`recovery.tanmatsu.cloud` and `ota.tanmatsu.cloud` are **two different
artifact sources**. Recovery is the WebSerial flasher used to get a
bricked or blank device back on its feet; OTA is what the running
launcher pulls when you open Tools → Firmware update.

| Source | Radio | Launcher |
|---|---|---|
| `recovery.tanmatsu.cloud` (WebSerial) | `ESP-HOSTED 2.1.0` | v0.1.2 |
| `ota.tanmatsu.cloud` (Launcher Tools) | v3.2.0 (`d83ad84`) | v0.1.7 |

Recovery currently lags the OTA path by several minor releases. The
`ESP-HOSTED 2.1.0` radio firmware predates the `tanmatsu-radio` rename
(so it's pre-v3.0.0); its `lora_protocol_config_params_t` struct is
smaller than the 16-byte form the MeshCore app expects, so
`lora_get_config` returns a response shorter than either of the
fallback lengths (24 or 25 bytes). The app surfaces this as **"LoRa
radio not available"**.

**The fix is always: after recovery, open Launcher → Tools → Firmware
update before installing MeshCore.** That pulls the OTA bundle, which
brings both pieces to v3.2.0 / v0.1.7, and MeshCore connects to the C6
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

**Net:** every firmware-side delta is now upstream. The only thing
still on a fork is the **launcher**, and only one of its three patches
(WiFi auto-connect, PR #141) is in flight to upstream.
