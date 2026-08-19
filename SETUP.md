# Set up Newsheen Radio

This guide describes what the device does at each step of setup, and lists the
assumptions the design depends on.

## Setup flow

### 1. Connect power

Plug the puck into USB-C. No app, account, or pairing is required.

| Output | Behavior |
| --- | --- |
| Ring | Slow amber pulse, meaning the device needs configuration |
| Audio | Announces the network name and spells out the password |
| Radio | Broadcasts the `Newsheen-Audio` access point |

The device speaks the credentials because it has no screen. A first-time user has
no other way to learn them.

### 2. Join the device's network

On a phone or laptop, join `Newsheen-Audio` with the password `meowmeow`.

The setup page opens automatically. The device runs a DNS server that resolves
every lookup to itself, which triggers the captive-portal prompt on iOS and
Android. If the page doesn't open, go to `http://192.168.4.1/`.

### 3. Select your Wi-Fi network

Open the **Wi-Fi** tab, select **Scan**, choose your network, enter the password,
and select **Save & connect**.

The device disconnects you for a few seconds. The ESP32-S3 has one radio, so its
access point must move to your network's channel. Your device reconnects on its
own.

| Output | Behavior |
| --- | --- |
| Ring | Blue chase while connecting, then one green sweep |
| Then | Your selected effect |

After it connects, the header shows the device's address on your network. You can
then leave `Newsheen-Audio`, return to your usual Wi-Fi, and reach the device at
`http://newsheen.local/`. The access point stays available at all times, so an
incorrect password or a changed network can't lock you out.

### 4. Play a station

Open the **Radio** tab, enter a station name, genre, or country, and select
**Search**, then **Play**. Select the star to save a favorite. Favorites persist
across power cycles.

Three other ways to start playback:

*   **Paste a stream URL.** Any `.mp3` or `.aac` stream works, with no directory
    involved.
*   **Use the globe.** Drag to spin, zoom with the buttons, and tap a marker to
    tune that station.
*   **Press the button.** One press mutes, two play the next favorite, three play
    a random station. No phone required.

### 5. Everyday use

*   **Effects tab.** 15 patterns with brightness, speed, and color controls. Four
    respond to audio.
*   **Files tab.** Upload MP3 files to play without an internet connection.
*   **Power cycle.** The device resumes the last station automatically.

## Assumptions

The setup flow depends on the following conditions. Checked items are verified on
this hardware. Check the unchecked items first when something doesn't work.

### Hardware

*   [x] The MAX98357A connects straight across to J3: `35_SDA` to LRC, `36_SCL`
    to BCLK, `37_WS` to DIN, `39_SD` to GAIN, `38_SCK` to SD, GND to GND, and 3V3
    to VIN.
*   [x] The module is an N16R2 with 16 MB quad flash and 2 MB quad PSRAM, which
    leaves GPIO33 through GPIO37 available. An N16R8 uses octal PSRAM and
    consumes those pins, which disables the header.
*   [ ] The U5 DIR modification is present. Without it, the level shifter runs
    backward and no LED effect is visible regardless of the firmware. This is the
    assumption most likely to be false on a given board.
*   [ ] The amplifier supply is adequate for sustained playback. J3 supplies 3V3
    from the shared regulator. For extended use at high volume, supply VIN from
    +5 V at U5 pin 6 or U4 pad 3, and add a bulk capacitor.

### Network

*   [ ] The network uses 2.4 GHz. The ESP32-S3 has no 5 GHz radio, so a 5 GHz
    network doesn't appear in the scan. This is the most common setup failure and
    it resembles a hardware fault.
*   [ ] The network uses WPA2 or is open. WPA3-only networks might refuse the
    connection.
*   [ ] The network has no captive portal or 802.1X login. The device can't
    complete a browser-based sign-in.
*   [ ] The network broadcasts its SSID. Hidden networks don't appear in the
    scan, but you can enter the name manually.

### Runtime

*   [x] The 192 KB stream buffer allocates in PSRAM. The boot log reports which
    memory it uses. A fallback to 32 KB of internal RAM causes audible dropouts.
*   [x] The buffer holds under load. In a 25-minute test the buffer stayed
    between 124 KB and 128 KB of 196 KB, with network input at 15.8–16.6 KB/s
    against decoder output of 15.1–15.5 KB/s and no underruns.
*   [x] Tasks that use sockets have adequate stack. The `netfill` task needs
    12 KB, not the 4 KB its own code suggests, because `NetworkClient::read()`
    runs the lwIP acknowledgment path and the Wi-Fi driver's 802.11 output on the
    calling task's stack, 38 frames deep. At 4 KB this triggered a stack canary
    panic about a minute into every stream. Measured headroom is now 7,924 bytes.
*   [x] No task starves another on the same core. The audio task yields on every
    pass. Without this, the priority-1 Arduino loop task never runs, which
    disables the web interface, the serial console, and the button during
    playback.
*   [ ] radio-browser.info is reachable. This affects search only. Favorites,
    pasted URLs, and the offline catalog work without it.
*   [ ] The selected station is online. Directory entries go stale. The firmware
    retries a dropped stream with backoff, then stops and reports the reason.

### Deliberate limitations

*   **HTTPS streams skip certificate validation.** Validating thousands of
    independent stations would require shipping and rotating a certificate
    bundle. The payload is a public broadcast and the device sends no credentials.
*   **The web interface has no authentication.** Anyone on your network can
    change the station. Don't expose the device to the internet.
*   **The access point password is fixed** at `meowmeow` and is spoken aloud
    during setup.
*   **Bluetooth audio isn't possible.** The ESP32-S3 has no Bluetooth Classic
    radio, and A2DP requires BR/EDR.

## Station sources

**radio-browser.info** provides search. It requires no authentication and
responds over plain HTTP, so search needs no TLS session.

**radio.garden** provides the optional offline catalog. Its API requires
browser-style request headers, including a `Referer` header. A request without
them returns HTTP 403. `tools/radio_garden_dump.py` walks its 12,465 places and
38,186 stations and resolves each to its upstream stream URL. Only those resolved
URLs work on the device, because the radio.garden redirect passes back through
the same API.
