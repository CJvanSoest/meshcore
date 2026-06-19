# Screenshots

Mock-ups of every view in landscape proportions (800x480). The
[README](../README.md) shows a few; the full set is here, each with a short note
on what you are looking at.

### Home — tile grid
The landing screen. A grid of tiles in LilyGo Pager style, one per view (Nodes,
DM, Channel, Map, Advert, Settings, About, QR). ESC steps back to home, ESC on
home returns to the launcher. Tiles carry their own unread badges.
<p><img src="../assets/screenshots/screen-home.svg" width="480" alt="Home tile grid"></p>

### Nodes
The live list of heard nodes: role, RSSI and SNR, distance, and last seen.
Saved contacts are starred and stay in the list even when out of range.
<p><img src="../assets/screenshots/screen-nodes.svg" width="480" alt="Nodes"></p>

### DM inbox
Pick a conversation. One row per contact you have a direct message thread with,
with an unread marker, plus the active target on top.
<p><img src="../assets/screenshots/screen-dm-inbox.svg" width="480" alt="DM inbox"></p>

### DM conversation
A per contact end to end encrypted thread. Message bubbles with local time, hop
count and ACK state under each one, and a typing line at the bottom.
<p><img src="../assets/screenshots/screen-dm.svg" width="480" alt="DM conversation"></p>

### Channel
Public channel chat (AES-128-ECB shared key) and user added `#channels`. Per
channel message rings, region scope in the header, and an unread badge.
<p><img src="../assets/screenshots/screen-channel.svg" width="480" alt="Channel"></p>

### Settings — category tiles
The first level of the two level Settings menu: a tile grid of categories
(Identity, Regulatory, Radio, Network, Region and Location, Brightness, Sounds).
Enter drills into one category's field list.
<p><img src="../assets/screenshots/screen-settings-tiles.svg" width="480" alt="Settings category tiles"></p>

### Settings → Radio
The drill-in field list for one category, here Radio: frequency, spreading
factor, bandwidth, coding rate, TX power, sync word, preamble and the LoRa
preset. The footer shows the active sub-band limits.
<p><img src="../assets/screenshots/screen-settings.svg" width="480" alt="Settings radio drill-in"></p>

### Settings → Brightness
Independent sliders for the display backlight, keyboard backlight and RGB LED,
plus the auto-blank timeout. These override the launcher globals while MeshCore
runs.
<p><img src="../assets/screenshots/screen-settings-brightness.svg" width="480" alt="Settings brightness drill-in"></p>

### QR contact card
The "add me as contact" overlay. Shows a QR the mobile MeshCore app can scan to
add this badge as a contact directly.
<p><img src="../assets/screenshots/screen-qr.svg" width="480" alt="QR contact card"></p>

### About
App version and build date, author, upstream credits, the MIT licence and the
source URL.
<p><img src="../assets/screenshots/screen-about.svg" width="480" alt="About"></p>

### Boot diagnostics
The cold start splash with per step diagnostic output (BSP, display, the P4 to
C6 link, the time source and the Ed25519 self-test) so a failed boot is visible
on screen.
<p><img src="../assets/screenshots/screen-boot.svg" width="480" alt="Boot diagnostics"></p>
