# Firmware versions — our forks vs. Nicolai (upstream)

The MeshCore app runs on top of three pieces of Nicolai-Electronics firmware.
For two of them we maintain a thin Gitea fork with a small delta; the launcher
fork is optional but improves day-to-day use. This page tracks exactly what
differs so the deltas stay auditable and easy to drop once they land upstream.

The on-device **Information** tab shows the live radio firmware version via the
launcher's system-protocol client.

## 1. `tanmatsu-radio` — C6 LoRa-radio firmware

| | Version | Commit |
|---|---|---|
| Nicolai (upstream) | v3.1.1 | `d1264bb` |
| Our fork | v3.1.1 **+1** | `f919f91` |

**Delta (1 commit):** `idf_component.yml` points `tanmatsu-lora` at our Gitea
fork instead of the registry, purely to pull in rx_boost. Otherwise identical to
stock v3.1.1. On-device version label: `v3.1.1-1-gf919f91`.

## 2. `tanmatsu-lora` — LoRa component (used by both C6 and P4)

| | Version | Commit |
|---|---|---|
| Nicolai (upstream) | v0.2.1 | `b9e87c5` |
| Our fork | v0.2.1 **+1** | `db18049` |

**Delta (1 commit): rx_boost.** Writes RxGain register `0x08AC` = `0x96`
(boosted, ~+3 dB sensitivity, ~+2 mA RX) instead of stock `0x94` (power-save),
with a config field so the app can toggle it (Settings → RX sensitivity). In the
field this is what yields "more nodes discovered, more messages received".

## 3. `tanmatsu-launcher` — P4 launcher

| | Version | Commit |
|---|---|---|
| Nicolai (upstream) | v0.1.4 | `c88b6bd` |
| Our fork | v0.1.4 **+3** | `9dd36e8` |

**Delta (3 commits):**
1. **WiFi auto-connect on boot** — upstream only connects when NTP is enabled;
   we connect unconditionally so apps have network after every reboot.
2. **Accept the whole v3.1.x radio line** — upstream hard-compares against
   `"v3.1.0"`, so a git-described radio (`v3.1.1-...`) is flagged as a mismatch
   and the risky "Update radio" downgrade tile appears. We prefix-match `v3.1.`,
   hiding both the false warning and the tile.
3. **Hide the `[C]` cached tag** in the app list (`[M]` mismatch tag stays).

## 4. `meshcore` (this app)

Not a Nicolai fork — entirely ours. It consumes the `tanmatsu-lora` component
(our fork, for rx_boost).

## What used to be ours but is now upstream (convergence)

- **RSSI/SNR + `GET_RSSI_INST`** → merged (radio PR #14 / lora PR #3).
- **RX-forward fix + esp-hosted callback limit** (issue #18) → merged in radio
  **v3.1.1** (`0ca17e3` + `MAX_CUSTOM_MSG_HANDLERS=6`).
- **System-protocol firmware-version query** → built upstream; this is what
  surfaces the radio version on the Information tab.
- **BSP 0.9.9 display types** (launcher) → fixed upstream (`7c86493`), so our
  earlier type-mismatch patch was dropped.

**Net:** the only durable deltas are **rx_boost** (radio + lora) and the **three
launcher UX patches**. Once rx_boost lands upstream, the radio and lora forks can
drop back to plain registry versions.
