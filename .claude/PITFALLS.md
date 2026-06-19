# Pitfalls

Traps that have already cost time or shipped broken behaviour on this project.
Each one is concrete. If you are about to do the thing on the left, read the
note first. See also [GUIDELINES.md](GUIDELINES.md) and Architecture.md.

## cppcheck `unusedFunction` lies about used code

`tests/lint/check-cppcheck.sh` runs `--enable=unusedFunction`, and it reports
many **live** functions as unused. Two patterns fool it:

- **Function-pointer tables.** The Settings UI installs handlers such as
  `save_brightness` into a field dispatch table by address
  (`render_settings.c`: `{ "Display backlight", save_brightness }`). cppcheck
  does not count taking an address as a call, so every `save_*` handler looks
  unused while in fact the UI invokes all of them.
- **Test-only callers.** Helpers like `region_effective_power_dbm` and
  `region_get_country_by_index` are exercised only from `tests/`, which the
  component scan does not include.

So `unusedFunction` is a **starting signal, not a verdict**. Before deleting
anything, grep the whole tree including `tests/`:

```sh
grep -rIn --include=*.c --include=*.h "\bsymbol_name\b" components main tests
```

If the only hits are the definition and the declaration, it is genuinely dead.
If there is a caller, a callback registration or a test, keep it. The full keep
list and the reasoning live in Architecture.md "Unused code".

## ed25519: two implementations, one correct, both compiled

`components/vendor/` ships **both** `ed25519.c` (ref10 style) and
`ed25519_mpi.c` (an mbedtls_mpi rewrite). They do not collide because they
define different symbols:

- `ed25519_sign` and `ed25519_create_keypair` are defined **only** in
  `ed25519_mpi.c`. This is the fix. The old ref10 sign produced wrong points
  for the RFC 8032 vector and every upstream verifier silently rejected it,
  which is why direct adverts looked dead.
- `ed25519_key_exchange` and `ed25519_pub_to_x25519` (the X25519 path used by DM
  crypto) are defined in `ed25519.c`.

So calling `ed25519_sign` is correct: the name resolves to the fixed
implementation. There is no separate `ed25519_mpi_sign` to call. Do not try to
"unify" the two files, and do not delete `ed25519.c` to remove the old sign:
its X25519 functions are still in use, and the broken sign symbol is already
gone. The leftover scalar helpers in `ed25519.c` that cppcheck flags are dead
ref10 remnants, kept because the file is vendored.

Both halves of advert signing are gated: the math by `test_ed25519` plus the
`identity_init()` boot self-test that calls `abort()` on mismatch, and the
signed byte layout by `test_advert_sign`. Keep both green.

## Flood and direct adverts share one signing path

`send_advert()` and `send_advert_direct()` both route through
`send_advert_internal()` in `mc_rx`, which has the single `ed25519_sign` call.
The `direct_route` flag only changes `msg.route` and `msg.path`, which are set
after signing and are not part of the signed bytes. So the two routes produce an
identical signature over identical bytes. Do not duplicate the signing logic per
route. The byte range that gets signed is built by the pure
`meshcore_advert_signable_bytes` (`mc_proto/advert_sign.c`) so it can be tested
without the radio.

## Time comes from the C6 RTC, not SNTP

`app_main` brings up the P4 to C6 stack but does not connect WiFi or run SNTP. It
reads the clock via `bsp_rtc_update_time`. There is no in-app SNTP path. If a
timestamp looks decades old, the bug is usually a tick count used where a UNIX
epoch was expected, not a missing time sync. An advert once shipped with
`xTaskGetTickCount`-derived uptime as its timestamp and other clients rejected it
because the time looked like ~15 seconds since 1970.

## Locking: the contacts table uses `node_mutex`

The contacts table is protected by `node_mutex`, not by a contacts-specific
mutex. `send_advert_direct` walks `contacts[]` under `node_mutex`. Hold the right
lock when touching either the nodes table or the contacts table, and keep the
hold short: TX happens outside the lock.

## Two radio firmware version strings track the C6 independently

`app_config.h` `TANMATSU_RADIO_FW_LABEL` and the string in
`radio_system_protocol_client.h` both describe the C6 firmware and they are
separate. Bump both together on a C6 reflash, or the version check disagrees
with itself.

## The two CI workflows must stay divergent on one line

`.github/workflows/ci.yml` and `.gitea/workflows/build.yml` are deliberately not
identical. The firmware build step differs:

- GitHub-hosted runners do not run the job in a container, so `$PWD` is a real
  host path and `docker run -v "$PWD":/project` works.
- The self-hosted Gitea `act_runner` runs the job inside a container with
  `bind_workdir: false`, so the workspace is a Docker volume. A host-path bind
  mount resolves to an empty directory on the outer daemon and the build sees no
  files. That workflow uses `--volumes-from "$(hostname)"` at `$GITHUB_WORKSPACE`
  instead.

Do not "sync" the two workflows to match. If you change the build step, change
it in the right file only and keep the other one as it is.

## Committed test binaries break CI

`tests/Makefile` builds binaries named `test_*` next to the `test_*.c` sources.
If a binary is committed, CI may run the stale binary instead of rebuilding,
and a host with a different libmbedcrypto soname fails to load it. `.gitignore`
already ignores `tests/test_*` while keeping `tests/test_*.c`. Never commit a
built test binary.

## Editing the box-drawing section headers

Several files use box-drawing rule comments such as
`// ── Section ─────────`. Those long runs of `─` are easy to mismatch in an
exact-string edit. Anchor edits on a unique nearby line of real code instead of
on the rule characters, or you will fight false "string not found" errors.

## "Make it consistent" is not a reason to touch vendored or mirror code

A sweep that renames, reformats or de-duplicates across the tree must skip
`components/vendor/*` and `components/mc_proto/meshcore/*`. Consistency there
means consistency with upstream, not with the rest of this repo.

## The RX task and the UI task share domain tables

`node_list` and `contacts[]` are both protected by `node_mutex`; the chat ring
by `chat_mutex`. The trap is that the LoRa RX handlers (`mc_rx`) run on the
`lora_rx` task while the UI mutates the same tables on the main loop. Every
domain read or write from an RX handler needs the lock, and the helpers in
`mc_domain` (`contact_ensure`, `contact_mark_unread`, the contacts scan,
`build_node_display`) take NO lock internally: the contract is caller-holds.
Real bugs found this way: the DM sender resolver scanned `contacts[]` unlocked
while a UI contact-removal shifted the array, and `rx_handle_dm` appended via
`contact_ensure` unlocked. Hold `node_mutex` around it. Keep the lock OFF the
expensive ed25519 work: take it to read/copy, release, then do the crypto.

## Shared accounting state touched by several TX tasks

`radio_tx_message` is called from at least four tasks (advert, direct-advert,
the RX-task PATH_RETURN ACK, UI sends). Anything it read-modify-writes must be
synchronised. The duty-cycle buckets + rolling sum were not, despite a comment
claiming a lock was held, so the sum could corrupt and let TX past the
regulatory budget. They are now under `dc_mutex`. If you add per-TX state,
synchronise it and do not trust a comment that says a lock is held: grep for the
actual `xSemaphoreTake`.

## A reused slot is a new identity, not an update

In `nodes.c update_node`, when the table is full a slot is evicted and reused.
The slot is still `active`, so `is_new = !active` wrongly read false and the new
node inherited the evicted node's name, packet count, position and stats.
Decide "new vs update" by comparing the slot's pubkey to the advert's, not by
the active flag, and zero the slot when a different identity takes it over. Any
fixed-capacity table with eviction has this trap.

## Keyboard and D-pad input paths must agree

Settings (and similar grid views) have two input paths: `nav_settings` (D-pad)
and `key_settings` (keyboard). They are separate code and drifted: the keyboard
path used the visible-slot cursor as the real category index (no
`settings_visible_category_real_idx` translation) and clamped with the real
count instead of the visible count, so keyboard Enter opened the wrong category.
When you change navigation in one path, change the other, or factor the shared
step into one helper.
