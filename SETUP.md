# Newsheen Radio — user flow and assumptions

## The flow

### 1. Power on

Plug the puck into USB-C. Nothing else is required — no app, no account, no
pairing.

| | |
|---|---|
| **Ring** | slow amber pulse = "I need setting up" |
| **Voice** | *"Join my wifi network, News Sheen Audio, password meow meow, to set me up."* |
| **Radio** | access point `Newsheen-Audio` is up |

The device says the credentials out loud because there is nowhere else a
first-time user could learn them — there's no screen, and a sticker is not
something firmware can ship.

### 2. Join the puck's network

On a phone or laptop, join **`Newsheen-Audio`** (password **`meowmeow`**).

The setup page **opens by itself**: the puck runs a DNS server that points every
lookup at itself, which is what makes iOS and Android show their "sign in to
network" sheet. If it doesn't appear, browse to **`http://192.168.4.1/`**.

### 3. Point it at your Wi-Fi

Wi-Fi tab → **Scan** → pick your network → password → **Save & connect**.

> **Expect to be dropped for a few seconds.** The ESP32 has one radio, so when
> it joins your network its own access point has to move to that network's
> channel. Your phone will reconnect on its own.

| | |
|---|---|
| **Ring** | blue chase while joining |
| **Then** | green sweep once, then your chosen effect |

Once it's online the header shows the puck's address on your home network. From
then on you can leave `Newsheen-Audio`, go back to your normal Wi-Fi, and reach
it at **`http://newsheen.local/`** — which is how you'd actually use it day to
day. The access point stays up permanently regardless, so a wrong password or a
changed network can never lock you out.

### 4. Play something

Radio tab → type a station, genre or country → **Search** → **Play**.
**★** saves a favourite; favourites survive a power cut.

Three other ways in:

- **Paste a stream URL** — any `.mp3`/`.aac` stream, no directory involved.
- **radio.garden** — drag *Send to Newsheen* to your bookmarks bar, then click it
  while a station plays on radio.garden. Your browser resolves the station and
  hands the puck the stream. (The puck can't talk to radio.garden itself; see
  "Why not radio.garden" below.)
- **The button on the puck** — short press walks your favourites, long press
  stops. No phone needed.

### 5. Live with it

- **Effects tab** — 11 patterns, brightness, speed, colour. Independent of audio,
  except *Audio VU* which follows whatever is playing.
- **Files tab** — upload MP3s to play from the puck itself, no internet needed.
- **Power cycle** — comes back on the last station automatically.

---

## Core assumptions

Everything below has to be true for the flow above to work. Ticked items are
verified on this hardware; unticked ones are the things to check first when
something misbehaves.

### Hardware

- [x] **MAX98357A wired straight across to J3** — `35_SDA→LRC, 36_SCL→BCLK,
      37_WS→DIN, 39_SD→GAIN, 38_SCK→SD, GND→GND, 3V3→VIN`.
- [x] **Module is N16R2** (16 MB quad flash, 2 MB **quad** PSRAM) so GPIO33–37 are
      free. An N16R8's octal PSRAM eats those pins and kills the whole header.
- [ ] **U5 DIR bodge is present** — without it the level shifter runs backwards
      and *every LED effect is invisible*, no matter what the firmware does.
      This is the single assumption most likely to be false on a given puck.
- [ ] **Amp supply is adequate for sustained playback.** J3's 3V3 is the shared
      AMS1117 LDO. Fine at moderate volume; for hours at high volume, feed VIN
      from +5 V (U5 pin 6 or U4 pad 3) and bulk-cap it.

### Network

- [ ] **The network is 2.4 GHz.** The ESP32-S3 has no 5 GHz radio. A 5 GHz-only
      SSID will not even appear in the scan. This is the most common setup
      failure and it looks like a broken device.
- [ ] **WPA2 (or open).** WPA3-only networks may refuse the join.
- [ ] **No captive portal or enterprise (802.1X) login.** Hotel, campus and guest
      networks that need a browser sign-in cannot be joined by the puck.
- [ ] **SSID is broadcast.** Hidden networks won't show in the scan; they can
      still be typed in by hand.

### Runtime

- [x] **192 KB stream buffer allocates in PSRAM.** Verified; the boot log says which.
      If it falls back to 32 KB of internal RAM, streaming will stutter.
- [x] **Buffer holds under load.** 7-minute soak: ring steady at 124–128 KB of 196 KB,
      network in 15.8–16.6 KB/s against decoder out 15.1–15.5 KB/s, zero underflows.
- [ ] **Heap survives a TLS handshake and a decoder at the same time.** HTTPS
      stations are the memory-hungry case; the boot log prints free heap.
- [x] **Any task touching a socket has a big enough stack.** `netfill` needs 12 KB,
      not the 4 KB its own code suggests: `NetworkClient::read()` runs the lwIP ACK
      transmit path and the Wi-Fi driver's 802.11 output on the *caller's* stack,
      38 frames deep. At 4 KB this panicked the device a minute into every stream.
      Measured headroom now: 7924 bytes free of 12288.
- [x] **No task starves another on the same core.** The audio task must yield each
      pass; without it the priority-1 Arduino loop task never runs and the web UI,
      serial CLI and button all go dead while audio plays.
- [ ] **radio-browser.info is reachable** — only needed for *search*. Favourites,
      pasted URLs and the bookmarklet all work without it.
- [ ] **The chosen station is actually up.** Directory entries go stale; the
      firmware retries a dropped stream three times, then gives up and idles.

### Deliberate limitations

- **HTTPS streams are fetched without certificate validation.** Validating a
  directory of thousands of independent stations would mean shipping and
  rotating a CA bundle. The payload is a public broadcast and nothing secret is
  ever sent, so this is a considered trade, not an oversight.
- **The web UI has no authentication.** Anyone on your Wi-Fi can change the
  station. That is the right call for a lamp and the wrong call for anything
  that matters — don't expose it to the internet.
- **The access point password is fixed** (`meowmeow`) and spoken aloud at boot
  when unconfigured.

### Station sources

**radio-browser.info** backs the search box: open, no auth, answers over plain HTTP so no TLS
session is needed just to search.

**radio.garden** is usable too — its API needs browser-shaped request headers (a `Referer` in
particular; a bare request gets HTTP 403, which is what led to an earlier note here claiming it
was blocked). `tools/radio_garden_dump.py` walks its ~12,465 places and ~30,000 stations and
resolves each to its real upstream stream URL. Only those resolved URLs are useful to the puck —
the radio.garden redirect goes back through the gated API and the device would get 403 on it.
