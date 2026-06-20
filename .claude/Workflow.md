# Workflow

How to carry a change from first read to a green commit on this project. The
rules behind each step are in [Guidelines.md](Guidelines.md); the traps are in
[Pitfalls.md](Pitfalls.md).

## 1. Understand before editing

- Read the relevant `docs/` page and the component the change touches. Trace the
  data flow in `docs/architecture/Components.md` so you edit where the cause is, not where the
  symptom shows.
- Decide the component and layer. If you cannot name them, you are not ready to
  write yet.
- Check whether the logic is pure (no ESP-IDF, pax or BSP). Pure logic belongs in
  `mc_proto` or a crypto component and gets a host test. Platform behaviour does
  not.

## 2. Make the change in the right place

- Put new first-party code in a component, never in `main/`.
- Keep `mc_radio` domain free and keep `meshcore/` a pure upstream mirror. New
  local behaviour goes in `mc_rx`, `mc_radio` or a pure helper in `mc_proto`
  root.
- New source file? Add it to that component's `CMakeLists.txt` `SRCS`, give it
  the SPDX header, and add the `SPDX-FileContributor` line for new authorship.
- Prefer extracting pure logic out of a non-testable function so a host test can
  reach it. `meshcore_advert_signable_bytes` exists for exactly this reason: the
  byte layout is now testable without the radio.

## 3. Add or update a host test

- Any pure logic you add or change needs a test in `tests/`. Wire it into
  `tests/Makefile` in three places: a `SRC_*` variable if it needs extra sources,
  a build rule, and both the `all:` and `test:` target lists plus `clean:`.
  `tests/lint/check-test-wiring.sh` fails the build if you miss one.
- For crypto and wire layout, prefer a golden vector: a fixed input and a
  hard-coded expected output, so a regression in either the logic or a dependency
  turns the test red. To generate the golden value, print it once, run the test,
  paste the bytes in, then switch the print to an assert.
- Tests link the real shipping `.c` files, not copies. A test passing proves the
  firmware code is correct, not a parallel implementation.

## 4. Run the full green gate locally

```sh
cd tests && make test
tests/lint/check-arch-rules.sh
tests/lint/check-structure.sh
tests/lint/check-test-wiring.sh
tests/lint/check-cppcheck.sh
```

All of these run without the ESP-IDF toolchain: they need only `gcc`,
`libmbedtls-dev` and `cppcheck`. Fix every failure before moving on.

## 5. Verify the firmware actually builds

A green host gate does not prove the firmware compiles and links. Build it.

```sh
make build DEVICE=tanmatsu
```

If you do not have the IDF toolchain installed, build in the same image CI uses:

```sh
docker run --rm -v "$PWD":/project -w /project espressif/idf:v5.5.1 \
  bash -lc '. /opt/esp/idf/export.sh && idf.py -B build/tanmatsu build \
    -DDEVICE=tanmatsu \
    -DSDKCONFIG_DEFAULTS="sdkconfigs/general;sdkconfigs/tanmatsu" \
    -DSDKCONFIG=sdkconfig_tanmatsu -DIDF_TARGET=esp32p4 -DFAT=0'
```

A comment-only or whitespace-only change to a `.c` file cannot break the build
and does not need a local IDF build, but anything that adds, moves or removes a
symbol does. When in doubt, build.

## 6. Commit

- Imperative subject, short body, at most a few lines. No AI attribution trailer,
  no co-author line.
- Pass the commit message through a file or careful quoting so shell escapes do
  not leak backslashes into the body.
- Group a change with its test and its doc update in the same commit where it
  makes sense.

## 7. Push and confirm CI

- After pushing, confirm the run is green before calling the work done. The
  firmware build job is the ground truth for compile and link.
- Report outcomes honestly. If a result was verified on hardware by someone else,
  say so rather than implying you ran it. If a step was skipped, say it.

## What the gates do and do not prove

| Gate | Proves | Does not prove |
|---|---|---|
| `make test` | Pure logic and crypto vectors are correct | Radio, display or C6 runtime behaviour |
| lint scripts | Layering, file placement, test wiring, no dead first-party code | Logic correctness |
| `make build DEVICE=tanmatsu` | The firmware compiles and links | That it runs correctly on the badge |
| Boot self-test in `identity_init()` | The shipped Ed25519 produces correct signatures at runtime | Anything else |
| Hardware smoke test | The feature works on a real badge | Nothing automatically; record what you tested |
