# Newsheen Radio

Newsheen Radio is ESP32-S3 firmware that turns a Retia Newsheen puck into an
internet radio with an audio-reactive light ring.

The firmware targets the `esp32_base_puck_v2` board with a MAX98357A I²S
amplifier connected to the J3 sensor header. It streams internet radio over HTTP
and HTTPS, searches an online station directory, and drives the puck's 8-pixel
ring from a live frequency analysis of the audio.

To install it without a toolchain, use [scriptkitty.sh](https://scriptkitty.sh/#newsheen).

## Features

*   **Streaming.** Plays MP3 and AAC/HE-AAC over HTTP and HTTPS on any port. The
    firmware follows redirects, resolves `.m3u` and `.pls` playlists, and strips
    Shoutcast and Icecast metadata to display the current track. A 192 KB PSRAM
    buffer absorbs network interruptions, and playback reconnects automatically.
    Recovery is verified against a forced 60-second outage.
*   **Station search.** Searches radio-browser.info by name, genre, or country.
    You can also paste a stream URL directly, and favorites persist across power
    cycles.
*   **Offline catalog (optional).** Build the radio.garden catalog with the
    included tools to store 37,963 stations from 227 countries on the device.
    Every entry includes coordinates. Browsing seeks into a byte-offset index on
    the filesystem, so it works without an internet directory. The catalog isn't
    part of the flashed image. See [Build the station catalog](#build-the-station-catalog).
*   **Globe.** Drag to spin, zoom with the buttons, scroll wheel, or a pinch
    gesture, and tap a marker to tune that station. The globe uses about 11 KB of
    Natural Earth coastline geometry with no map tiles and no external requests,
    so it renders while you're connected to the puck's own access point.
*   **Light effects.** 15 effects, 4 of which respond to audio: Spectrum, Pulse,
    Aurora, and Sparkle, plus a peak-hold VU meter and 10 clock-driven patterns.
*   **Local playback.** Plays MP3 files from the device filesystem, speaks
    through the SAM synthesizer, and plays a built-in chiptune.

## Set up the radio

1.  Connect power. The ring pulses amber and the device announces how to reach
    it, including the access point password.
2.  Join the `Newsheen-Audio` network with the password `meowmeow`. The setup
    page opens automatically. If it doesn't, go to `http://192.168.4.1/`.
3.  Select your Wi-Fi network and enter its password.

    The device disconnects you for a few seconds while it joins. The ESP32-S3 has
    one radio, so its access point must move to your network's channel.

    After it connects, reach the device at `http://newsheen.local/`.

The access point stays available at all times, so an incorrect password can't
lock you out.

## Button controls

| Gesture | Action |
| --- | --- |
| Press once | Mute or unmute. When offline, repeats the setup instructions. |
| Press twice | Play the next favorite. |
| Press three times | Play a random station. |
| Press and hold | Ramp the volume up and down. Release to set it. The ring shows the level. |
| Hold BOOT for 5 seconds | Arm a Wi-Fi reset. Press the round button to confirm. |

The Wi-Fi reset requires two buttons because GPIO0 also carries the USB DTR
signal. A serial monitor that asserts DTR would otherwise erase your network
settings.

## Wire the amplifier

The MAX98357A breakout and the puck's J3 header both have 7 pins. When you align
them in silkscreen order, GND meets GND and VIN meets 3V3, so the default build
connects them straight across:

| J3 pin (silkscreen order) | MAX98357A pin |
| --- | --- |
| `35_SDA` | LRC |
| `36_SCL` | BCLK |
| `37_WS` | DIN |
| `39_SD` | GAIN |
| `38_SCK` | SD |
| `GND` | GND |
| `3V3` | VIN |

The ESP32-S3 routes I²S through its GPIO matrix, so these pins don't need to be
the ones labeled I²S on the silkscreen. To wire flying leads to the labeled pins
instead, build the `newsheen-speaker-classic` environment.

**Caution:** J3 supplies 3V3 from the same regulator as the microcontroller. Add
a bulk capacitor across the amplifier's VIN. For higher volume, supply VIN from
+5 V at U5 pin 6 or U4 pad 3 instead.

**Caution:** This wiring requires the N16R2 module. The N16R8 module uses octal
PSRAM, which consumes GPIO33 through GPIO37 and makes the header unusable. Run
`esptool flash-id` to confirm which module your board has.

The 8 NeoPixels require the U5 DIR modification common to all Pusheen pucks. The
GPIO48 debug LED follows the audio regardless, so you can confirm the firmware
runs on an unmodified board.

## Build the firmware

```bash
pio run -e newsheen-speaker
pio run -e newsheen-speaker -t buildfs
```

The build requires an arduino-esp32 3.x platform. ESP8266Audio 2.4 includes the
IDF5 I²S driver, which doesn't compile against the 2.0.x platforms that other
puck projects use.

## Build the station catalog

```bash
cd tools
python3 radio_garden_dump.py --probe
python3 radio_garden_dump.py --resolve
python3 build_catalogue.py
```

`build_catalogue.py` writes `data/stations.tsv` and `data/countries.idx`. Flash
them with `pio run -t uploadfs`.

Only the resolved upstream URLs work on the device. The radio.garden redirect URL
requires browser-style request headers that the firmware doesn't send.

## Play audio from a Mac

`tools/mac_cast.py` serves Mac audio as an MP3 stream that the device can tune:

```bash
python3 tools/mac_cast.py --list
python3 tools/mac_cast.py --input :0
```

Then tune `http://<your-mac-ip>:8100/live` on the device.

To capture system audio rather than a microphone, install a loopback driver such
as BlackHole and select it with `--input`.

**Note:** The ESP32-S3 doesn't support Bluetooth Classic, so this device can't
work as a Bluetooth speaker. A2DP requires BR/EDR, which the radio hardware
doesn't implement.

## Serial console

Connect at 115200 baud and enter `help` for the command list. Commands include
`status`, `stats`, `tasks`, `tune`, `search`, `press`, `ramp`, and `netkill`.

## Known issue: HTTPS stations can crash the device

Tuning an HTTPS station can panic and reboot the device. It happens at decoder
start, right after the stream prebuffers, and it is intermittent rather than
every time. Plain HTTP is unaffected: 30 consecutive plain-HTTP station changes
run clean, while HTTPS fails within 2 to 10, and sometimes on the first tune.
About half the station directory is HTTPS.

The device recovers on its own. It reboots, and the boot guard forgets the saved
station after three starts without stable playback, so it cannot get stuck in a
crash loop.

The root cause is not yet found. Three theories have been tested on hardware and
disproven: multichannel AAC frames, the audio task stack size, and heap
corruption. `RESEARCH-BRIEF.md` records what was ruled out and how.

To reproduce it, run `tools/racetest.py`; `tools/plaintest.py` is the plain-HTTP
control.

## Documentation

*   [SETUP.md](SETUP.md) describes the complete setup flow and lists the
    assumptions the design depends on.
*   [RESEARCH-BRIEF.md](RESEARCH-BRIEF.md) records measured performance figures,
    resolved defects, and open problems.

## License

MIT.

This project bundles ESP8266Audio and ESP8266SAM by Earle F. Philhower, III,
Adafruit NeoPixel, and ArduinoJson. Coastline geometry derives from Natural Earth
public domain data. Station data comes from radio.garden and radio-browser.info.
This project isn't affiliated with either service.
