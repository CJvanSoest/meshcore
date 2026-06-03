# Firmware versions — our forks vs. Nicolai (upstream)

The MeshCore app runs on top of three pieces of Nicolai-Electronics
firmware. The radio + lora component now have **zero durable deltas**:
the rx_boost feature we used to carry was merged upstream and tagged in
`tanmatsu-lora` v0.3.0 on 2026-06-03. The launcher fork still carries
three small UX patches.

The on-device **Information** tab shows the live radio firmware version
via the system-protocol `get_information` query introduced in
`tanmatsu-radio` v3.1.1.

## 1. `tanmatsu-radio` — C6 LoRa-radio firmware

| | Version | Commit |
|---|---|---|
| Nicolai (upstream) | **v3.1.1** | `d1264bb` |
| Our fork (legacy)  | v3.1.1 **+1** | `f919f91` |
| `recovery.tanmatsu.cloud` | `ESP-HOSTED 2.1.0` (pre-v3.0.0 rename) | — |
| `ota.tanmatsu.cloud` (Launcher Tools) | v3.1.1 | `d1264bb` |

**Standard install:** stock upstream **v3.1.1**, as shipped by the
Launcher's Tools → Firmware update path. No fork required.

**Why we still have a fork commit:** our `f919f91` only redirects
`idf_component.yml` to a Gitea-hosted `tanmatsu-lora` fork so the build
could pull in rx_boost before it was upstream. Now that rx_boost is in
upstream lora v0.3.0, this redirect is obsolete and the fork will be
retired the next time we rebuild the C6 firmware against the registry
component.

## 2. `tanmatsu-lora` — LoRa component (used by both C6 and P4)

| | Version | Commit |
|---|---|---|
| Nicolai (upstream) | **v0.3.0** | `bdda8be` |
| Previous upstream (radio v3.1.1 still ships this) | v0.2.1 | `b9e87c5` |
| Our fork (legacy) | v0.2.1 + rx_boost | `db18049` |

**v0.3.0** (released 2026-06-03) contains:
- `rx_boost` config flag + RxGain register write (our PR #5, merged
  2026-06-03 as `4cc38f95`)
- Force `low_data_rate_optimization` when SF/BW require it (`bdda8be`)

**Status of our fork:** convergent with upstream. The 17-byte config
struct is the same shape on both sides. The app currently still pins
the Gitea fork in `idf_component.yml`; switching to
`nicolaielectronics/tanmatsu-lora ^0.3.0` is a one-line change pending
a clean rebuild + retest.

**Note about radio v3.1.1:** the OTA radio bundle was built against
v0.2.1 and ships the 16-byte config form. The app's `lora_get_config`
client accepts both 24-byte (v0.2.1) and 25-byte (v0.3.0+) responses,
so the same MeshCore build works on both until Renze rebuilds the radio
firmware against v0.3.0.

## 3. `tanmatsu-launcher` — P4 launcher

| | Version | Commit |
|---|---|---|
| Nicolai (upstream) | **v0.1.6** | `2201f7d` |
| Our fork | v0.1.4 **+3** | `9dd36e8` |
| `recovery.tanmatsu.cloud` | v0.1.2 | — |
| `ota.tanmatsu.cloud` (Launcher Tools) | v0.1.6 | `2201f7d` |

**Standard install:** stock upstream **v0.1.6** (from Tools → Firmware
update). Stock v0.1.6 expects `tanmatsu-radio` v3.1.1 exactly, which is
what OTA ships, so the version-mismatch screen stays off.

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

Not a Nicolai fork — entirely ours. Consumes `tanmatsu-lora` (currently
still our Gitea fork, switching to upstream v0.3.0 is pending).

## Recovery vs. OTA — why "Tools → Firmware update" is required after recovery

`recovery.tanmatsu.cloud` and `ota.tanmatsu.cloud` are **two different
artifact sources**. Recovery is the WebSerial flasher used to get a
bricked or blank device back on its feet; OTA is what the running
launcher pulls when you open Tools → Firmware update.

| Source | Radio | Launcher |
|---|---|---|
| `recovery.tanmatsu.cloud` (WebSerial) | `ESP-HOSTED 2.1.0` | v0.1.2 |
| `ota.tanmatsu.cloud` (Launcher Tools) | v3.1.1 (`d1264bb`) | v0.1.6 (`2201f7d`) |

Recovery currently lags the OTA path by several minor releases. The
`ESP-HOSTED 2.1.0` radio firmware predates the `tanmatsu-radio` rename
(so it's pre-v3.0.0); its `lora_protocol_config_params_t` struct is
smaller than the 16-byte form the MeshCore app expects, so
`lora_get_config` returns a response shorter than either of the
fallback lengths (24 or 25 bytes). The app surfaces this as **"LoRa
radio not available"**.

**The fix is always: after recovery, open Launcher → Tools → Firmware
update before installing MeshCore.** That pulls the OTA bundle, which
brings both pieces to v3.1.1 / v0.1.6, and MeshCore connects to the C6
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
  v0.3.0 (`56671b79`, our PR #5, 2026-06-03).
- **BSP 0.9.9 display types** (launcher) → fixed upstream in `7c86493`,
  so the earlier type-mismatch patch was dropped.

**Net:** every firmware-side delta is now upstream. The only thing
still on a fork is the **launcher**, and only one of its three patches
(WiFi auto-connect, PR #141) is worth upstreaming.
