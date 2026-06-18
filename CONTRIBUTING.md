# Contributing

Thanks for helping out with MeshCore for Tanmatsu. This is a community build,
not the official MeshCore app; bug reports and feature ideas go to
[the issue tracker](https://github.com/CJvanSoest/meshcore/issues).

## Build and test

```sh
make build  DEVICE=tanmatsu      # idf.py build → build/tanmatsu/application.bin
make upload DEVICE=tanmatsu      # badgelink AppFS upload (keeps the launcher)

cd tests && make test            # host gcc tests; CI runs these before the IDF build
```

Set up the Tanmatsu ESP-IDF toolchain first (see
[docs/wiki/Build-Deploy.md](docs/wiki/Build-Deploy.md)). Other board targets
live in `sdkconfigs/`.

## Before you open a PR

1. **`cd tests && make test` is green.** Host tests link against the shipping
   translation units, so a crypto, parser, regulatory or packet-codec
   regression fails here.
2. **`tests/check-arch-rules.sh` is green.** It enforces the layer rules from
   [ARCHITECTURE.md](ARCHITECTURE.md) (CI runs it too).
3. **`make build DEVICE=tanmatsu` is green.**
4. **`clang-format -i` on the files you touched** (`.clang-format` is the
   source of truth).
5. **Behaviour that only shows on the badge is tested on a badge.** The host
   tests and the IDF build cannot prove runtime behaviour on the radio or the
   display; flash it.

## Code rules

These keep the codebase reasoned-about rather than just compiling. Full detail
in [ARCHITECTURE.md](ARCHITECTURE.md) and [CLAUDE.md](CLAUDE.md).

- **Respect the layers.** Higher layers include lower, never the reverse.
  `render_*.c` does not speak the wire protocol. The component `REQUIRES`
  graph (`components/mc_proto`, `components/vendor`, `main`) enforces this at
  build time, and `check-arch-rules.sh` covers the in-`main` layering.
- **`components/mc_proto/` is the upstream protocol mirror.** Keep it free of
  ESP-IDF / pax / BSP includes (C stdlib and POSIX only) so it stays
  host-testable. Do not extend a wire-format struct locally; take it upstream
  first, then re-pin the dependency. When the compiler warns inside the mirror,
  suppress it at the call site, not by editing the mirror.
- **`components/vendor/` is third-party code.** Do not refactor it or "fix" its
  TODOs; most TODO markers in the tree are upstream LodePNG comments.
- **Add a host test for any pure logic you add or change.** Modules with no
  ESP-IDF dependency (the protocol codecs, region limits, the GPS and companion
  parsers) are unit-tested on the host and gate the merge. Keep that property.
- **Comments explain the non-obvious** (a wire quirk, a locking coupling), not
  the obvious. Every source file carries an SPDX header.

## Commit and PR style

- Commit messages and all repository text are in English.
- One logical change per commit; explain the why, not just the what.
- PRs target `main`. CI must be green. See the PR template under `.gitea/`.

## License

By contributing you agree your work is released under the project's MIT
license (see [LICENSE](LICENSE)).
