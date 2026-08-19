# Newsheen Radio

Internet radio, a 38,000-station atlas, and an audio-reactive light show — inside
a silicone cat lamp.

Firmware for the **Retia Newsheen puck** (`esp32_base_puck_v2`, ESP32-S3) with a
MAX98357A I²S amplifier on the sensor header. It streams internet radio over
HTTP and HTTPS, carries the entire radio.garden catalogue on its own flash so
browsing works without any directory service, and drives the puck's 8-pixel ring
from live frequency analysis of whatever is playing.

Flash it from **[scriptkitty.sh](https://scriptkitty.sh/#newsheen)** — no
toolchain required.

---

## What it does

**Radio.** MP3 and AAC/HE-AAC, plain HTTP and TLS, on any port. Follows
redirects, resolves `.m3u`/`.pls` playlists, strips Shoutcast/Icecast metadata
and shows the now-playing title. Rides out network outages from a 192 KB PSRAM
buffer and reconnects on its own — verified against a forced 60-second drop.

**38,000 stations, offline.** The full radio.garden catalogue lives on the
device: 37,963 stations across 227 countries, every one with coordinates.
Browsing is a seek into a byte-offset index on LittleFS, so it needs no internet
directory at all. Text search additionally uses radio-browser.info when online.

**A globe.** Drag to spin, scroll or pinch or use the +/− buttons to zoom, tap a
light to tune it. Natural Earth coastlines, ~3.9 KB of geometry, no map tiles and
no CDN — it renders while you're joined to the puck's own access point.

**Fifteen light effects**, four of them audio-reactive: *Spectrum* (three bands
around the ring), *Pulse* (beat-triggered bloom), *Aurora* (slow colour field),
*Sparkle* (treble strikes), plus a peak-hold VU meter and ten classic patterns.

**Also** plays MP3s from its own flash, speaks (SAM synthesiser), and sings an
original chiptune.

## Setting it up

1. **Power it.** The ring breathes amber and it says how to reach it — including
   spelling out the password, because there's no screen to read it from.
2. **Join `Newsheen-Audio`** (password `meowmeow`). The setup page opens itself;
   otherwise browse to `http://192.168.4.1/`.
3. **Point it at your Wi-Fi.** Expect a few seconds of disconnection — one radio
   can't serve an access point and join a network on two different channels at
   once. Afterwards it's at `http://newsheen.local/`.

The access point never goes away, so a wrong password can't lock you out.

## The button

| Gesture | Action |
|---|---|
| 1 press | Mute / unmute · **offline:** repeat the setup instructions |
| 2 presses | Next favourite |
| 3 presses | Random station (filtered to what the hardware can sustain) |
| Hold | Volume ramps up and down — release to set. The ring is the slider. |
| BOOT held 5 s | Arms a Wi-Fi reset; confirm with a press of the round button |

The reset needs two buttons on purpose: GPIO0 doubles as the USB DTR line, so a
serial monitor holding DTR would otherwise wipe your network settings by itself.

## Wiring

The MAX98357A breakout and the puck's J3 header are both 1×07, and laid side by
side in silkscreen order **GND meets GND and VIN meets 3V3** — so the default
build wires straight across with no crossed leads:

| J3 (silk, top → bottom) | MAX98357A |
|---|---|
| `35_SDA` | LRC |
| `36_SCL` | BCLK |
| `37_WS` | DIN |
| `39_SD` | GAIN |
| `38_SCK` | SD |
| `GND` | GND |
| `3V3` | VIN |

The ESP32-S3 routes I²S through its GPIO matrix, so it doesn't matter that these
aren't the pins the silkscreen calls I²S. Build `-e newsheen-speaker-classic` if
you'd rather wire flying leads to the labelled pins.

**Two things before you solder.** J3's 3V3 comes from the same LDO as the MCU;
bulk-cap the amplifier's VIN, and for real volume feed it from +5 V (U5 pin 6 or
U4 pad 3) instead. And this assumes the **N16R2** module — an N16R8's octal PSRAM
consumes GPIO33–37 and kills the header entirely. Run `esptool flash-id` first.

The 8 NeoPixels need the U5 DIR bodge that every Pusheen puck needs; the GPIO48
debug LED follows the audio regardless, so an un-bodged puck still shows life.

## Building it yourself

```bash
pio run -e newsheen-speaker                     # firmware
pio run -e newsheen-speaker -t buildfs          # filesystem (see below)
```

Needs an **arduino-esp32 3.x** platform — ESP8266Audio 2.4 pulls in the IDF5 I²S
driver, so the 2.0.x platforms other puck projects use will not compile it.

To rebuild the station catalogue:

```bash
cd tools
python3 radio_garden_dump.py --probe        # check reachability first
python3 radio_garden_dump.py --resolve      # ~38k stations, resumable
python3 build_catalogue.py                  # -> data/stations.tsv + countries.idx
```

Only the **resolved upstream URLs** are usable by the device; the radio.garden
redirect URL goes back through an API that refuses non-browser clients.

## Extras

`tools/mac_cast.py` turns a Mac into a station the puck can tune, so you can play
Mac audio through it. (Bluetooth is not possible — the ESP32-S3 has no Bluetooth
Classic, so A2DP cannot work on this hardware.)

A serial console at 115200 exposes everything: `help`, `status`, `stats`, `tasks`,
`tune`, `search`, `press`, `ramp`, `netkill`.

## Documentation

- **[SETUP.md](SETUP.md)** — the user flow in full, plus every assumption the
  design depends on and which of them are verified.
- **[RESEARCH-BRIEF.md](RESEARCH-BRIEF.md)** — measured baselines, the expensive
  bugs and what caused them, and the problems still open.

## Licence

MIT. Bundles ESP8266Audio and ESP8266SAM (Earle F. Philhower, III), Adafruit
NeoPixel, and ArduinoJson. Coastline geometry derived from Natural Earth (public
domain). Station data from radio.garden and radio-browser.info; this project is
not affiliated with either.
