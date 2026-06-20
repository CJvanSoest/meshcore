# Build and CI

How the firmware builds, the board targets and CI. The everyday change loop is in [Workflow.md](Workflow.md).

## Local build

```sh
make build  DEVICE=tanmatsu      # idf.py build -> build/tanmatsu/application.bin
make upload DEVICE=tanmatsu      # badgelink AppFS upload (keeps the launcher)
cd tests && make test            # host tests, no IDF toolchain needed
```

The Tanmatsu ESP-IDF v5.5.1 toolchain must be set up first (see
[docs/guides/Build-Deploy.md](../docs/guides/Build-Deploy.md)). The default board is tanmatsu
(ESP32-P4 app processor + ESP32-C6 radio). Other targets live in `sdkconfigs/`
(mch2022, hackerhotel-2024, heltecv3, kami, konsool,
esp32-p4-function-ev-board) and are selected through the `SDKCONFIG_DEFAULTS`
chain.

## Building without the toolchain

The same image CI uses works locally via Docker:

```sh
docker run --rm -v "$PWD":/project -w /project espressif/idf:v5.5.1 \
  bash -lc '. /opt/esp/idf/export.sh && idf.py -B build/tanmatsu build \
    -DDEVICE=tanmatsu \
    -DSDKCONFIG_DEFAULTS="sdkconfigs/general;sdkconfigs/tanmatsu" \
    -DSDKCONFIG=sdkconfig_tanmatsu -DIDF_TARGET=esp32p4 -DFAT=0'
```

A comment-only or whitespace-only change to a `.c` file cannot break the build.
Anything that adds, moves, or removes a symbol must be built before you trust
it.

## CI

`.github/workflows/ci.yml` runs on every pull request to `main` and on `main`
itself: a host tests job (the four lint scripts + `make test`, needing only
`build-essential libmbedtls-dev cppcheck`) and a firmware build job that builds
the tanmatsu target in the `espressif/idf:v5.5.1` image and uploads
`application.bin`.

## The lint gate

Four scripts under `tests/lint/`, run in CI before the build and easy to run
locally:

- `check-arch-rules.sh` enforces the include-direction discipline (grep-based,
  mirrors the `REQUIRES` graph).
- `check-structure.sh` keeps `main/` thin and the repo root clean (the allowed
  top-level dirs are listed inside it; add new ones there).
- `check-test-wiring.sh` fails if a `tests/test_*.c` is not wired into the
  Makefile.
- `check-cppcheck.sh` runs cppcheck over the first-party components. Treat its
  `unusedFunction` output as advisory, see [Pitfalls.md](Pitfalls.md).

## Artifacts and partitions

The build emits `build/tanmatsu/application.bin`, uploaded by CI as an artifact.
It lives in the app partition (a 2 MB AppFS slot); keep an eye on free space
when adding code or assets. `make upload` writes via badgelink AppFS and leaves
the launcher intact.

## Commit and push expectations

Imperative subject, short body, no AI-attribution trailer. After pushing,
confirm the run is green; the firmware build job is the ground truth for compile
and link. Report honestly: if a result was only checked on hardware by someone
else, say so.
