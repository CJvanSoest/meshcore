# Building a MeshCore Client on the Tanmatsu Badge

*What I learned building an embedded LoRa mesh networking app from scratch*

---

## What I Built

The [Tanmatsu](https://tanmatsu.cloud) is an open-source badge with an ESP32-P4 processor, a separate ESP32-C6 radio co-processor for LoRa, and a 4" display. I built a native [MeshCore](https://meshcore.co.uk) client for it: a LoRa mesh networking protocol that lets devices broadcast their presence, exchange group messages, and send encrypted direct messages. The iOS/Android app already exists; I wanted the badge itself to be a first-class node on the network.

The end result: four views (Settings, Nodes, DM, Channel), live node discovery, group chat, encrypted direct messages with delivery confirmation, and time-synced timestamps.

---

## What I Learned

### 1. Read the source before writing a line of code

My first instinct was to start coding. My first lesson was to stop and read: the protocol documentation, the BSP driver code, the existing C library. Half the bugs I encountered in the first week were things that were already documented somewhere I hadn't looked yet.

The BSP input system is a good example. I spent time wondering why Enter didn't work in my list view, only to discover that navigation keys (Enter, Tab, arrow keys) come through a completely different callback than regular keyboard characters. It was in the driver. I just hadn't read that part yet.

### 2. Hardware abstraction hides real complexity

The Tanmatsu uses two chips that work together: the application processor handles the display, input, and app logic; a separate radio chip handles LoRa. They communicate over a bus, and the boundary between them has real rules that aren't obvious from the high-level API.

I learned this when I tried to update the radio firmware from within the app. Without properly shutting down the communication layer first, the application processor watchdog fired and the whole device rebooted. The fix was three lines, but finding those three lines required understanding what was actually happening at the hardware level, not just at the API level.

The same lesson appeared with WiFi scanning: the initialisation function leaves the radio stopped. Call `scan_start()` right after init and you get zero results with no error. Read the source and you see why.

### 3. Cryptography punishes assumptions

The MeshCore DM protocol uses elliptic curve cryptography for key exchange and AES encryption for message content. The existing Ed25519 implementation I was building on had a subtle bug in its field arithmetic: a multiplication function that used the wrong variable in six places.

There was no crash. No error message. Just a wrong answer, silently.

This taught me something important: in cryptographic code, correctness can't be inferred from the program running without errors. You need mathematical self-checks (computing the same value two different ways and verifying they agree) built into the initialisation. That's how I found it, and that's the only way I would have found it.

### 4. Reverse engineering is a skill worth developing

The MeshCore protocol documentation covers the basics, but some details, like exactly how a delivery acknowledgement is structured, aren't written down anywhere. I had to work backwards from how the receiver (the iPhone app) responded to different packets I sent.

This was slow and sometimes frustrating, but it taught me to think in terms of bytes: what does each field mean, in what order, with what encoding? When the iPhone finally showed "delivered" after I got the acknowledgement format right, it was one of the most satisfying moments of the whole project.

### 5. Time is a resource on embedded systems

On a device without a battery-backed clock, power-off means losing track of time. Boot always starts at Unix epoch zero. For a messaging app, this breaks message ordering on the receiver's side.

The solution (syncing via SNTP on startup and persisting the timestamp to flash) was straightforward once I understood the problem. But I hadn't thought about it at all before I encountered it. Embedded development constantly surfaces constraints that simply don't exist in web or mobile development.

### 6. The standard library is already there: know what's in it

ESP-IDF ships with mbedTLS, SNTP, NVS (non-volatile storage), and a lot more. Several times I started thinking about how to implement something, then discovered it was already available and well-tested. The challenge isn't usually writing the code; it's knowing where to look.

---

## Reflecting on the Process

This project pushed me into areas I hadn't worked in before: network protocols, field arithmetic, hardware co-processor communication. Each one had a learning curve that was steeper than I expected.

What made it manageable was breaking it into layers: get the UI working first, then the simple protocol messages, then the hard cryptography. Each layer could be tested independently, and each gave me a foundation to stand on when tackling the next one.

If I were starting again, I'd spend more time upfront understanding the hardware and protocol before writing any application code. The time I "saved" by jumping in early was paid back with interest in debugging time later.

---

## What I Learned in the Next Iteration

After the first version was running, I spent another week adding features and sanding down rough edges. A few of those lessons felt worth writing down.

### 7. Names lie, registers don't

The LoRa chip's driver exposes a constant called `SX126X_LORA_BANDWIDTH_62`. I read that as 62 kHz and assumed my Tanmatsu was running at a different bandwidth than a peer device documented as 62.5 kHz. I was about to spend an hour digging into a mismatch that didn't exist.

The register value behind that constant is `0x03`, which maps to 62.5 kHz on the SX1262. The `_62` in the symbol name is just a rounded label. Both nodes were running at exactly the same bandwidth.

The lesson: when something looks like a discrepancy between two systems, go back to the lowest-level documentation you have access to — the register tables, not the symbol names that wrap them. Names are written for humans, and humans round things off.

### 8. What a library doesn't do matters as much as what it does

The MeshCore C library I'm using is a clean, focused protocol codec. It serialises packets, decrypts payloads, and walks the wire format. It does not deduplicate.

I didn't think about that until people started seeing the same channel message two or three times on the badge. That's flood routing working as designed: multiple repeaters forward the same packet via different paths and they all arrive at me. My app wasn't filtering them out.

The fix was a 32-entry ring buffer of payload fingerprints. The meta-lesson is more useful than the fix: when you adopt a library, build a mental model of what it explicitly handles versus what it leaves to you. Both matter equally, and the second list is the one that bites you.

### 9. Fingerprint the stable part, not the wrapper

A subtler version of the deduplication problem: which bytes do you actually hash to recognise a duplicate?

I almost hashed the raw packet bytes. That would have failed, because flood retransmits change the header — the path field updates as the packet hops through the network. The bytes that stay constant are the encrypted payload, because the sender produced the ciphertext once and AES-ECB is deterministic given the same plaintext and key.

The lesson generalises beyond mesh networks: in any caching or deduplication, your key has to be derived from the parts that are invariant under the kind of variation you want to ignore. Spend a minute thinking about that before you reach for `memcmp`.

### 10. Set the timezone before anything reads time

When I added local-time display, I called `setenv("TZ", "CET-1CEST,...")` just before `esp_sntp_init()`. It worked when the badge had WiFi.

It quietly broke when WiFi was down. The badge restores its last known time from NVS instead, and that code path ran before my `setenv`, so `localtime_r` returned UTC. A two-hour offset, visible to users, only on the offline path.

This is the classic "happy path tested, edge path broken" bug. The lesson: setup code that affects global state should run before *any* branch that might use it, not just before the branch you were thinking about when you wrote it. Moving the two lines up by ten lines in `app_main` was a one-minute fix; finding it took longer.

### 11. Be honest about what a setting actually does

I added a "Role" setting that lets the user pick between Chat / Repeater / Room Server / Sensor. Underneath, all it does is flip a flag in the outgoing advertisement packet — the badge doesn't actually retransmit other nodes' traffic if you pick Repeater.

That's a useful feature: it changes how other nodes display this node in their list. But it would be misleading if the UI didn't say so. I added a tooltip: *"Role: advertised only. Does not enable repeater behavior."* Five seconds of writing prevented hours of users wondering why their "repeater" wasn't repeating anything.

The lesson: where a setting's name implies more than it delivers, name the gap explicitly in the UI. Honest tooltips are cheap; misled users are expensive.

---

## What I Learned During the Restyle Week

A few days spent making this app visually consistent with another project on a different badge (WHY2025). The lessons aren't the kind you find in a style guide.

### 12. A colour palette is an API between apps

The four tabs of the meshcore-app now share the same Tokyo Night palette as my LoRa-info app on the WHY2025 badge. I copied the hex values one-for-one, kept all existing `COL_*` names, and only replaced the numbers behind them. Ninety-nine call-sites stayed untouched.

The lesson: if you want two apps to feel like part of the same family, treat the colour palette as a shared definition rather than as a scattering of incidental hex values. The discipline that makes a public API stable — naming, fewest moving parts, no accidental coupling — applies to design tokens too.

### 13. Pick a font for your most-shown glyph, not your favourite word

I switched from a monospace bitmap font to a proportional one (Saira). It looks better in prose — but its "1" glyph is thin, and at small sizes it's hard to distinguish from a lowercase "l". In an app full of RSSI values, SNR readings, and pagination counters like "1-8/11", that's a real legibility bug, not a cosmetic gripe.

The fix was unglamorous: bump numeric fields up one type-size step. The lesson is broader. When you choose a font, test it on the characters your app actually displays most often. A font that reads beautifully in a paragraph of running text can still be wrong if your screen is mostly digits.

### 14. Persistent UI requires persistent data

After a restart, the DM tab stopped showing the list of people I'd been chatting with. My first instinct was that the view was broken. The view was fine — the data wasn't there. Contacts only got saved when the user explicitly favourited them, never automatically as a side effect of actual DM traffic.

The fix was one small, idempotent helper (`contact_ensure`) called from every code path that creates a DM relationship: receiving a message, sending a message, and opening a chat from the node list. Three call-sites, no new storage format, no new schema.

The lesson: when a UI is supposed to remember something, the fix lives in the storage layer, not the render layer. If you find yourself trying to make the view "remember" something, you've usually misdiagnosed the bug.

### 15. Reuse before you rebuild

The DM inbox presented a classic fork in the road. Option A: extend the storage layer with per-contact buffers and per-contact files on SD. Option B: a thin UI overlay on top of the existing globally-shared chat buffer.

Option A was the architecturally tidier choice. Option B got me 80% of the UX win for maybe 10% of the work, and crucially it didn't block doing Option A later if it ever turns out to matter. I picked B. Per-contact storage is now an explicit item on the roadmap, but it stays out of this release.

The lesson is one I keep relearning: incremental improvements are fine — as long as they don't foreclose the bigger step you might want to take later. Default to the smaller, reversible move.

---

## What I Learned About Text Input and Protocol Archaeology

A short sprint to make the badge's owner name editable, give the LoRa advertisement a separate name, and dig into why direct messages from a peer device kept failing in one specific mode. Four lessons came out of it.

### 16. One text-input helper, multiple fields

The owner name and the new LoRa advertisement name both needed to be editable. Rather than build two parallel edit modes, I added one editing state (`field_editing_text`) and one edit buffer, with two thin helpers — `settings_begin_text_edit(field)` and `settings_commit_text_edit(field)` — that pick which field gets loaded and saved. The keystroke handler doesn't know about fields at all; it only knows "text is being typed, write to the buffer."

The payoff: adding a third editable field is now a single case statement, not a new input mode. Shared edit state with per-field load/commit hooks scales better than a mode per field, and stays clean as the field list grows.

### 17. One source of truth, three visible places

After adding the separate advertisement name, scanning the badge's QR code still showed the old owner name on the other device. The fallback logic ("use advertisement name if set, else owner name") was baked into `send_advert()` and nowhere else. The fix was to synchronise the same fallback across three call-sites: `send_advert`, the QR overlay (both the URL `name=` parameter and the on-screen label), and the channel chat prefix.

The lesson: once a derived value is computed in more than one place, that's a symptom. Extract the choice once and use it everywhere — otherwise you'll guarantee at least one site that gets forgotten the next time the logic changes.

### 18. Acknowledging is not the same as teaching the route back

For every incoming direct message, the badge was sending a PATH_RETURN reply with `path_length=0`, regardless of how many repeaters the message had crossed on the way in. The remote node could therefore never learn a reverse path and had to flood every subsequent message. Fix: take the inbound `path[]`, reverse it, and embed it in the encrypted inner data of the PATH_RETURN (padded to a 16- or 32-byte AES block, HMAC over the ciphertext).

The lesson generalises: in a mesh network, an acknowledgement isn't the same thing as giving the other side a usable route back. Both ends of a conversation need a working path, or you keep paying for floods on every single message.

### 19. Hex dump first, documentation second

Direct messages from a peer device started failing the moment I switched its "path hash size" setting from 1 byte to 2. Our deserialiser was reading the second byte of the packet as a path length of 64 — invalid, dropped. I temporarily extended the RX log to dump the full hex payload. Side by side with a working 1-byte packet, the rest of the bytes were identical: same destination, source, MAC, ciphertext at the same offsets. Only the second byte differed: `0x00` vs `0x40`.

That observation alone was enough to conclude that `0x40` is a flag bit packed into the path-length byte, not a literal 64-hop path. Mask became `& 0x3F` — lower six bits as length, top two bits ignored. No upstream-source archaeology needed.

The lesson: when you suspect a field is being misread, dump the raw wire format and compare with a known-working variant. That comparison is faster and more certain than reading the spec.

### 20. A fix that looks right is not the same as one that works end-to-end

The mask change above was elegant, justified by the hex dump, and built cleanly. In real-world testing, direct messages in 2-byte mode still failed. There is clearly more to the protocol mode change than just one flag bit. I added the unresolved case to the backlog as an explicit open item rather than papering over it with a half-fix.

The lesson: if the end-to-end test doesn't pass, your fix isn't finished. An explicit open issue is better than a silently broken code path that looks healthy because the unit-level check is green.

---

## What I Learned About Refactoring, Self-Healing Data, and Documentation

Two weeks after the v2 release: a big cleanup, one stubborn bug that only appeared after an NVS erase, and a documentation sprint to lock in everything that had changed.

### 21. A file that's no longer reviewable is a design failure

`main.c` had grown to about 3000 lines — protocol parsing, NVS helpers, rendering, input handling, SD-card history, identity management, all in one place. I split it into nine modules with a clear theme per file (`history`, `contacts`, `identity`, `settings_nvs`, `nodes`, `chat`, `radio`, `input`, `render`) plus a `ui_state.h` for the shared `extern` declarations. Zero behavioural change — pure organisation.

The result: `main.c` is 320 lines that only do `app_main()` (boot, init, event loop), and everything else is navigable per concern. The lesson: when a file is so big you can't find anything without `grep`, that *is* the refactoring signal. Don't wait for a functional excuse.

### 22. A key you no longer have blocks you forever

After an NVS erase (accidentally triggered by flashing a new partition table), the app started up with a freshly generated Ed25519 identity. The chat history on the SD card was encrypted with an AES key derived from the *old* private key. Symptom: new DMs arrived, the notification LED blinked, but the text never appeared in the chat view — and after a restart it was still missing.

Diagnosis from the serial log: the load loop failed at record 0 with `bad magic` and stopped, so new records were never loaded either; the old garbage at the start of the file kept blocking everything behind it. Fix: a `fatal` flag in the load loop; if the file is unreadable *from record 0* the whole file is removed, so the next append starts a fresh log under the current key. Readable prefixes are preserved (a corrupt tail is left alone) — only fully unreadable files get wiped.

The lesson generalises: persistent data encrypted with a key that lives on a different persistence plane needs a recovery path when that key disappears. Otherwise old ciphertext blocks the door forever.

### 23. A launcher setting you assume is on, sometimes isn't

During the post-v2 testing the badge launcher got an update and the Tanmatsu picked up a new WiFi behaviour: it stopped auto-reconnecting after every reboot. The root cause in the upstream launcher: `wifi_connect_try_all()` was only called when NTP was enabled. Reasonable for many users (no NTP = no need for WiFi), but for me a launcher upgrade had silently flipped the default for a behaviour I depended on.

Local patch in our launcher checkout: always try WiFi, leave the NTP path independent. Not pushed upstream — it's a project-specific assumption — but documented in a memory file so I can check at every launcher update whether the patch is still needed.

The lesson: for upstream dependencies whose defaults shape your runtime behaviour, keep a list of your local patches *and why*. Otherwise you'll spend months puzzled by "it worked yesterday."

### 24. Docs are a snapshot, not a sequel

After the refactor and the self-heal fix, the existing SVG screenshots and README were no longer accurate. A new round: six SVGs (boot, settings, nodes, DM, channel, QR, radio-bootloader) in the current Tokyo Night palette, README updated with the module table and all the new settings fields and a Makefile quick-start, and a `docs/wiki/` set up with eight markdown pages per topic (architecture, protocol, UI, NVS, SD card, C6 radio, build).

I chose markdown files in the repo over a real `.wiki.git` — they render directly on Gitea/GitHub and can be promoted with `cp` to a separate wiki repo later if I want one.

The lesson: documentation doesn't keep itself current. After a big refactor, block out a deliberate docs sprint *before* you build the next thing. Otherwise the wiki sits months later describing a UI that no longer exists.

---

## What I Learned About Scale-Factor Archaeology and Half-Migrated State

A distance column for the Nodes tab seemed like a small addition — we have our own GPS coords + per-node positions from adverts. First test: a T-Beam in the same room, same coordinates as the Tanmatsu → distance reading: 5211 km.

### 25. Comments lie, source code doesn't

My `nodes.h` had `// degrees × 1e7` as a comment next to the int32 lat/lon fields. I built the entire input/output pipeline on that assumption. One `grep "/1000000" upstream/` later: MeshCore upstream uses ×1e6 (`AdvertDataHelpers.h::getLat() = _lat / 1000000.0`). The comment was wrong at some point in the past and everyone — including me — trusted it.

The lesson: for protocol fields, don't build on a comment in your own header. Verify in the upstream source or in a sniffer dump. A wrongly assumed scale doesn't show up as decimal formatting weirdness; it cascades through every calculation downstream.

### 26. Renaming by sed is cheap, scale mismatches expensive

When I rolled out the fix, the change touched 6 spots: text-edit parse + display, settings row display, advert TX, and the Nodes-Dist haversine. `sed`-rename `gps_lat_e7` → `gps_lat_e6` was trivial; the actual mental load was finding every numeric literal `1e7` / `1e-7`. The compiler can't catch that — the type is still int32.

The lesson: when you rename a scale, also rename the constants right away, not just the symbol. Otherwise the bug stays in numeric form, invisible to the type checker.

### 27. Half-migrated NVS is worse than frozen-wrong NVS

My first migration code said: "if lat > 90M, divide lat by 10." But the user sat at (51.87°N, 5.29°E). In ×1e7 that's lat=518718190 (triggers!) but lon=52919140 (< 180M lon threshold, does NOT trigger). Result: lat correctly migrated to 51.87°, lon stayed at 52.92° = somewhere in Central Asia → 3269 km.

Second attempt "if lat > 90M, divide BOTH axes by 10" fixed *new* half-migrations, but the user's NVS was already corrupted into a visually valid state (both < 180M). No heuristic could detect that anymore.

Final fix: an NVS sentinel key `lora.gps.sv` (u8 scale version). On boot, if the key is missing or below the current version → wipe the GPS keys and force the user to re-enter once. `save_gps_coords` always writes the current version. Future scale changes: bump the version.

The lesson: scale migrations with per-field trigger conditions can produce half-migrations; use a sentinel-version key instead of value heuristics. Frozen-wrong is recoverable through a manual reset; half-migrated looks valid and can't be detected after the fact.

### 28. A user typo under a scale bug compounds the confusion

After the migration bug looked "solved", the user still reported 3269 km. My reflex: add more migration code. Only after a fresh screenshot did it turn out that the longitude Settings value had been entered as 52.919140 (typo: 52.9 instead of 5.29). The value I'd diagnosed as "half-migration corruption" was actually a manual typo from after the migration.

The lesson: before launching a second round of correction code, actively ask for the current state — Settings screenshot, NVS dump, whatever's concrete. Otherwise you're building solutions for a problem that no longer exists.

---

## What I Learned About Inspecting Upstream Before Reverse-Engineering

Same long session: unread counters, GPS coords, ACK-tracking, channel-list UI, and wire-side region scope. The themes that stuck:

### 29. A screenshot is a better spec than a description

The user shared photos of their own backlog list AND of the iPhone MeshCore app's channel-chat header. Without those references I'd either have guessed the UI hierarchy wrong or spent hours doing protocol RE to build "something like that". For next sessions: actively ask for visual references when the user describes a target look. It's far less work than reverse-engineering, and it locks expectations in advance.

### 30. Always read upstream source before planning RE

For the region-scope wire format I was planning a sniffer setup with the T-Beam — until one `grep -rn "RegionMap"` in the upstream MeshCore repo revealed the entire mechanism: `helpers/RegionMap.h` + `TransportKeyStore.cpp::calcTransportCode` + `companion_radio/MyMesh.cpp::sendFloodScoped`. The wire layout was `ROUTE_TYPE_TRANSPORT_FLOOD + transport_codes[2]` with `code = HMAC-SHA256(SHA256(region_name)[0..15], type || payload)[0..1]`. Implementation: one helper call before serialize.

The lesson: "I don't know how this works" usually means "I haven't read the source yet."

### 31. Every input-event route must support text-input modes

The user reported "Enter does nothing on Add channel". My handler worked for `\r/\n` via the keyboard route, but `BSP_INPUT_NAVIGATION_KEY_RETURN` (D-pad center) went through a separate handler that had `!channel_adding` as a guard, skipping the save path.

The lesson: every mode that does text input MUST be registered in *all* event paths (keyboard + nav). One grep on your mode flag should match in both handlers — if the flag exists on one path and not the other, something's off.

### 32. Header context beats inline text prefix

In the previous session I added `[#test] name: text` prefixes to disambiguate channels in the chat ring. Once the chat header gained real channel context (channel name + `Region: nl` stacked, modeled after the iPhone screenshot), the inline prefix became redundant and visually noisy. Removed from 4 places (TX×2, RX×2).

The lesson: redundant visualizations pile up during iteration — periodic cleanup is part of the work. Or better: every time you add a new display affordance, ask "which old one does this obsolete?"

### 34. Programmatic drawing vs. real bitmaps

First take at emoji: hand-drawn with `pax_simple_circle` + `pax_simple_arc`.
Technically worked, but the result looked like yellow blobs with vague
features. Second take: Twemoji 32×32 ARGB bitmaps embedded as
`static const uint32_t[]` arrays in flash. `pax_buf_init` wraps each
array in a `pax_buf_t`; `pax_draw_image_sized` scales them to inline
text size at hardware-accelerated speed.

The lesson: for pixel-art with expression, an asset pipeline
(download + downscale + embed) is cheaper than drawing primitives.
Programmatic drawing is fine for schema-icons (arrows, checks); for
character glyphs that need to be *recognised*, it's not.

### 35. UTF-8 cross-platform is free when you actually do UTF-8

The wire format for emoji is just the Unicode codepoint as 4-byte
UTF-8. No custom protocol, no tokens, no separate payload type. An
emoji-aware receiver (iPhone app) renders the glyph; a non-aware
receiver shows a `?` or nothing — but the message stays intact.

The lesson: for protocol features that are optional on the receiver
side, pick a wire format that degrades to "do nothing" instead of
"broken message" on legacy clients.

### 36. Two input paths means two handlers

First flash test: emoji picker selection via D-pad didn't work, and
the picker hung around into the next typing session. Diagnosis:
keyboard Enter delivers `\r` to `handle_key`; D-pad center delivers
`BSP_INPUT_NAVIGATION_KEY_RETURN` to `handle_nav`. My handler only
lived in `handle_key`.

The lesson: every mode that takes text-input or navigation MUST be
registered in *all* event paths that can toggle it. One grep on the
mode flag should match in both handlers — if one path is missing the
mode is operable from only one input method.

### 37. Ask the hardware, not your intuition

First pick for the emoji shortcut: `BSP_INPUT_NAVIGATION_KEY_GAMEPAD_A`.
Build passed, nothing happened on the badge. Turns out the Tanmatsu
BSP doesn't map the coloured buttons to GAMEPAD_* at all — they're
`F1..F6`. Launcher `home.c` line 355 confirms: F4 is the green circle.

The lesson: BSP key-mappings differ per badge target. Look at a
shipping app (the launcher) to learn the real mapping, not the generic
enum names.

### 38. App-icon: AppFS has no icon slot

The custom tile icon doesn't work via `badgelink appfs upload` — that
protocol only carries slug/title/version/size. The launcher's
`app_metadata_parser` reads icon + metadata from `<slug>/metadata.json`
on SD. So the path is a SD bundle (binary + metadata.json + icon32.png
under a per-slug folder) — and that's also exactly the bundle a future
appstore would distribute.

The lesson: having a second install route alongside the fast-dev AppFS
upload doesn't just enable the tile icon, it lays the foundation for
later distribution. Not duplication — evolution.

### 33. ACK matching via CRC binding to the sender

For the "ack" indicator on outgoing DMs: at send_dm_message I compute the ACK CRC the receiver will echo back (`SHA256(plaintext[0..5] || dm_text || OUR_pubkey)[0..4]`), store it on the chat_msg, and add a PATH_RETURN inbound handler that decrypts the inner block with the sender's shared secret and matches on that CRC. Two things I learned: (1) MeshCore's inner ACK is derived from the plaintext + receiver's pubkey, so we can compute it deterministically in advance. (2) Building a sender that *transmits* PATH_RETURN is not the same as a sender that *receives* PATH_RETURN — they're separate paths, and I only had the TX side. Adding the RX side was a separate ~80 lines.

## What I Learned About Regulatory Compliance and Upstreaming

### 39. Regulation is a layer, not a checkbox

A community user pointed out that transmitting outside the permitted band or above the power cap carries real fines — tens of thousands of euros in some countries. The fix was a `Country` field that loads the right limits. The key detail: the EU 863–870 MHz band is not a single limit but **six sub-bands** (g/g1/g1'/g2/g3/g4), each with its own power and duty-cycle ceiling. Meshtastic collapses each region to one sub-band; I keep the full granularity so the check is correct for the frequency you actually use. The lesson: regulation in software is a data layer at the granularity of the real rules — not a single "max power" constant.

### 40. Soft-warn where it's the operator's call; hard-block where it harms the mesh

Frequency off-band or a few dB over the limit → red row + warning, but allowed (you might have a licence or a directional antenna). Duty cycle → hard enforced: a rolling 1-hour airtime budget (Semtech time-on-air formula, 60 one-minute buckets), and TX is **blocked** once the budget is spent. The difference is that airtime hogging actively degrades the shared mesh, so that limit isn't the operator's to bend.

### 41. A metric doesn't always measure what you think

Enabling RxBoost (`0x96` to RxGain register `0x08AC`) noticeably improved reception in the field — but the noise floor stayed identical. Why? The SX1262 reports calibrated/absolute RSSI referenced to the antenna input, so the extra LNA gain is compensated out by the chip. The win is in weak-signal decode (packets over time), not the noise-floor number. The lesson: verify a feature with the metric that actually moves, not the first one that seems plausible — otherwise you conclude "doesn't work" when it does.

### 42. A fork is a delta, not a destination

My RSSI/SNR contribution merged upstream at Nicolai Electronics. Rather than let my Gitea forks drift, I rebased them onto the merged upstream so only **two** commits of delta remain (rx_boost + a firmware-version query) — features not yet upstream. The thinner the delta, the more trivial the next upstream bump. Treat a fork as a temporary overlay on upstream, not a parallel universe.

### 43. Keep display order == enum order to avoid a navigation refactor

Section headings in a 22-field settings list sounded like a cursor-logic refactor. The trick: keep the enum order equal to the display order and keep `selected` a field index. Headings then become pure render artifacts (non-selectable) — the input code doesn't change. Only scrolling had to move to pixel-based (with clipping) so the shorter heading rows scroll with the list. Decouple presentation from navigation by keeping the order aligned, not by adding state.

### 44. Ephemeral UI state shouldn't force a persistence migration

Per-conversation unread counters and scroll position live in RAM-only parallel arrays, not in the contacts/channels NVS blobs. Adding a struct field there would have corrupted old data — that code derives the item count from the blob size. Not everything per-item needs to persist; weigh a format migration against simply resetting on reboot.

### 45. Put help where the doubt is

The footer now shows a short explanation per selected field — Sync word and Preamble explain themselves, and Country/Frequency/TX power/Duty cycle surface the active sub-band's limits (range, max dBm ERP/EIRP, % duty). Contextual micro-help on the field itself beats a separate manual nobody opens.

## What I Learned Syncing to a Refactored Upstream

### 46. `esp_err_t` in a bare `if` is an inverted-logic trap

After switching to the refactored radio firmware, nothing was received. The C6 *was* receiving (its console showed `preamble → header → Data available!`), but the host got no packets. The culprit: `if (lora_receive_packet(...))`, where that function returns `esp_err_t` — and `ESP_OK == 0`. So on a successful receive the condition was `if (0)` → false → never forwarded. One `== ESP_OK` and RX worked. A function with a "0 = success" convention does not belong bare in an `if`: it reads as "if success" but means "if failure".

### 47. Capture at the layer where you suspect the bug

All I knew was "no RX" — which could live anywhere (radio, transport, host parsing). Reading the C6 console while a neighbour node transmitted showed the radio + interrupt + read-queue all working. That collapsed the search space instantly: everything before the forward was fine, so the bug was *in* the forward. Measure as close to the suspicion as you can, and confirm what *does* work — that's as informative as what doesn't.

### 48. `ESP_ERR_NO_MEM` isn't always the heap

The first blocker was subtler: the esp-hosted slave has a fixed custom-callback table (`MAX_CUSTOM_MSG_HANDLERS = 3`), but the new firmware registers six protocol servers. The first three fit; the rest — including LoRa, registered last — fell off with `ESP_ERR_NO_MEM`. The boot log read like "out of memory", but it was a full fixed array. Read the line above (`No space for callback (max 3)`) before you start chasing the heap.

### 49. Syncing against a moving upstream: fix locally, report precisely

The maintainer was actively refactoring that same day (v3.1.0, a system protocol, launcher updates). Both of our fixes are one-liners: applied locally in our fork to keep moving, and reported immediately as an issue with reproduction + boot log so they get fixed upstream instead of living forever in our fork. With a fast-moving upstream, patch locally to unblock, but report right away with enough detail that the maintainer can take it over.

### 50. A successful bug report makes your own patch redundant — that's the goal

The maintainer merged both reported firmware bugs upstream (radio v3.1.1: the RX fix as `0ca17e3 "Fix receiving LoRa packets (issue #18)"`, the callback limit as `MAX_CUSTOM_MSG_HANDLERS=6`). Both of our fix commits were now redundant, so the radio fork shrank from three delta commits to **one** (just the redirect to our lora fork for rx_boost), rebased onto v3.1.1. The success of an upstream contribution isn't measured in lines you add, but in lines you can later remove.

### 51. Equal file size defeats naive mismatch detection

The app binary changed only in one version string — exactly the same length, hence the same size. The launcher detects a new version by revision-or-size; both unchanged → it kept running the old cache. Fix: explicitly clear the AppFS cache so it reinstalls fresh from SD. A cache invalidation keyed on "size or revision" misses precisely the change that touches neither.

### 52. Hardcoded version checks rot silently

The launcher hard-compared the radio against `"v3.1.0"`; our git-described radio reports `v3.1.1-1-g…` → a false mismatch, and the dangerous "Update radio" downgrade tile appeared. Our launcher fork now prefix-matches `v3.1.`, hiding both the warning and the tile. Compare firmware versions at a meaning level (line/range), not against an exact string that every build changes.

### 53. Three forks, three sizes of delta

End state: radio = upstream + 1 (rx_boost redirect), lora = upstream + 1 (rx_boost), launcher = upstream + 3 (WiFi auto-connect, version-accept, hide the `[C]` tag). Everything that could converge has converged; what remains are deliberate features. Document each fork's delta and *why* — then "can this go back upstream?" stays an answerable question instead of an archaeological one.

### 54. Same markdown, two renderers, two link conventions

A new wiki page wouldn't open: a bare link `(Firmware-Versions)` works in a wiki, but in the GitHub/Gitea **repo file browser** it resolves to a path without an extension → 404. The `.md` form (`(Firmware-Versions.md)`) works in both. Bonus trap: the rendered Gitea wiki turned out to be a separate git repo that must be synced by hand — the source `docs/wiki/` had been ahead of it for months. Know which renderer your docs are viewed through and test a link in *that* context — "it renders" isn't "the links work".

### 55. The schema is the source of truth, not the README

Packaging for the Tanmatsu app store (PR to `Nicolai-Electronics/app-repository`), the README said `license` and `target`, but the actual validator (`.validator/schema.json`, Ajv) required `license_type` and `targets` — and the name/description regex forbids `:` and `,`, both of which were in our description. `additionalProperties:false` meant an extra `repository` field would have failed. Validating locally against that schema (Python `jsonschema`) before the PR caught everything in one pass instead of via a CI failure round. Validate against the machine-readable source (schema + a working example), not the prose around it.

### 56. A first-time contributor's PR waits at the gate

The PR opened fine, but the metadata CI didn't run — GitHub holds Actions on a first-time contributor's PR until a maintainer approves them (the CLA check did run automatically). Not a failure, just the rule. When checks are *missing* rather than *failing* on a fork PR, it's usually approval-gating, not broken CI — wait for the maintainer instead of debugging what isn't broken.

---

## Links

- Source: [github.com/CJvanSoest/meshcore](https://github.com/CJvanSoest/meshcore)
- Tanmatsu badge: [tanmatsu.cloud](https://tanmatsu.cloud)
- MeshCore protocol: [meshcore.co.uk](https://meshcore.co.uk)

---

*Developed by CJ van Soest with [Claude AI](https://claude.ai) as co-author.*
