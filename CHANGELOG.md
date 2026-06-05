# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/).

Sections per release:
- **Added** — new features
- **Changed** — changes in existing functionality
- **Deprecated** — soon-to-be removed features
- **Removed** — removed features
- **Fixed** — bug fixes
- **Security** — vulnerabilities and mitigations

If a section for a release is missing from this file when running
`scripts/release.sh`, the release notes fall back to a categorized list
of merged PR titles since the previous tag.

## [Unreleased]

### Added
- Gitea Actions CI workflow (`.gitea/workflows/build.yml`) — builds the
  tanmatsu target on every push and PR, uploads `application.bin` as
  artifact (PR #1).
- PR template (`.gitea/PULL_REQUEST_TEMPLATE.md`) — five sections:
  What / Why / Test plan / Breaking changes / Screenshots (PR #2).
- `ARCHITECTURE.md` at repo root — six-layer model, three forbidden-include
  rules, wire-boundary discipline for upstream-compat (PR #3).
- Upstream-tracking comment on `radio_system_protocol_client.h` —
  demonstrates the per-file boundary-tracking pattern.
- `scripts/release.sh` — single command for cutting a release on Gitea
  + GitHub with changelog + binary attached.

### Changed
- Branch protection rule on Gitea `main`: direct push disabled, CI
  status check required, block-on-outdated enabled. Devlog files
  exempted via `unprotected_file_patterns`.

## [2.2.0] - 2026-06-04

### Added
- `VIEW_HOME` tile-grid landing screen in Pager-stijl — eight tiles
  with PAX-drawn icons (Nodes, DM, Channel, Map, Advert, Settings,
  About, QR).
- Settings drilldown — six category tiles (Identity, Regulatory,
  Radio, Network, Region & Location, Brightness); each opens a focused
  field list.
- Per-app brightness controls — three sliders (display / keyboard /
  LED), cycling through 5/10/25/50/75/100%, persisted in NVS.
- `VIEW_ABOUT` — pulls version from `esp_app_get_description()`, plus
  authors, credits, license, source URL.
- Pager-style status header on every view — view name + inline
  DM/channel unread badges + RX/TX/battery.

### Changed
- Render dispatcher rewritten to a single-flush model — every
  `render_*()` writes the framebuffer without blitting; the dispatcher
  blits exactly once at the end. Kills the v2.1.x QR + emoji overlay
  flicker.
- `render.c` split per view: `render_settings.c`, `render_nodes.c`,
  `render_chat.c`, `render_channel.c`, `render_home.c`, `render_about.c`.
  Cross-file declarations in `render_internal.h`.
- `tanmatsu-lora` dependency switched from CJ Gitea fork to upstream
  `nicolaielectronics/tanmatsu-lora ^0.3.0` (rx_boost landed upstream).
- Radio FW label bumped to `v3.2.0` (upstream `tanmatsu-radio`).

### Fixed
- QR overlay close hint clarified to `[ESC] [X] [Enter] to close`.
- Advert tile no longer dimmed as "soon" placeholder — it has an
  action (`HOME_ACTION_SEND_ADVERT`), so it's live.

[Unreleased]: http://192.168.2.25:3000/CJ/meshcore/compare/v2.2.0...HEAD
[2.2.0]: http://192.168.2.25:3000/CJ/meshcore/releases/tag/v2.2.0
