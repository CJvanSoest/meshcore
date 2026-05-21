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

## Links

- Source: [github.com/CJvanSoest/meshcore](https://github.com/CJvanSoest/meshcore)
- Tanmatsu badge: [tanmatsu.cloud](https://tanmatsu.cloud)
- MeshCore protocol: [meshcore.co.uk](https://meshcore.co.uk)

---

*Developed by CJ van Soest with [Claude AI](https://claude.ai) as co-author.*
