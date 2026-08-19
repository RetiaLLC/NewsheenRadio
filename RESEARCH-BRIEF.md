# Newsheen Radio — research brief

For an agent picking up open problems on this firmware. Everything below is
measured on the hardware unless marked otherwise. Read `SETUP.md` for the user
flow and `README.md` for the build.

## What this is

ESP32-S3 internet radio in a silicone cat lamp. Retia Pusheen puck
(`esp32_base_puck_v2`), N16R2 module (16 MB quad flash, **2 MB quad PSRAM**),
MAX98357A I2S amp on the J3 header, 8× WS2812B ring, one user button (GPIO17)
plus the BOOT strapping pin (GPIO0). Arduino / pioarduino core 3.x,
ESP8266Audio for the decoders, hand-rolled HTTP.

Not built on `schreibfaul1/ESP32-audioI2S` — that decision predates knowing how
good it is, and revisiting it is one of the open questions below.

## Measured baseline (do not re-derive)

Directory composition, top 300 stations on radio-browser:

| property | value |
|---|---|
| codec | MP3 217, AAC 38, AAC+ 27, OGG 3, unknown 15 (**MP3+AAC ≈ 94%**) |
| scheme | 155 https / 145 http |
| playlists | 32/300, of which **30 are `.m3u8` (HLS)** |
| geo coords present | ~30% of rows (radio.garden has coordinates for all ~30k of its stations) |

Device throughput: **the earlier "~23 KB/s / ~190 kbps ceiling" figure was wrong
and is withdrawn.** It was measured against internet radio stations, so it
recorded the stations' pacing and load, not the device. Against an unthrottled
320 kbps stream served from a Mac on the same LAN the device reached **47 KB/s
(~376 kbps)**. Disabling Wi-Fi modem sleep remains a genuine, reproducible
doubling (13.2 → 20 KB/s).

**Do not benchmark this device against internet stations.** Two failures came
from it: measuring config A then config B against a station whose own load
drifted produced a convincing but fake 40% improvement, which vanished on
re-measurement. Serve a high-bitrate file from the LAN instead, so the decoder
always wants more than the link can supply and `net in` reports the device.

**Also check RSSI before and after every run.** On this bench it drifted from
−35 dBm to −88 dBm between sessions (12 dB of spread within a single 10-sample
window), which collapses throughput to under 1 KB/s and invalidates any
comparison silently. The `netfill` core-affinity question (below) is still
**open** for exactly this reason.

Task layout: `audio` decode prio 2 core 1 (32 KB stack, ~3.5 KB used),
`netfill` producer prio 3 core 0 (12 KB stack, ~4.4 KB used), LEDs core 0,
Arduino `loopTask` core 1 serving HTTP. 192 KB PSRAM ring, 48 KB prebuffer.

25-minute soak on an AAC+ TLS station: heap flat at ~76 KB free (67 KB min-ever),
PSRAM flat, stacks flat, ring full, zero underruns, zero reconnects, zero crashes.

## Solved — context for anything you touch

These were expensive to find; don't reintroduce them.

1. **Stack overflow in the network task.** `NetworkClient::read()` runs the lwIP
   TCP-ACK *transmit* path and the Wi-Fi driver's 802.11 output **on the calling
   task's stack**, 38 frames deep. 4 KB tripped the canary. Size any
   socket-touching task for the whole network stack, not your own code.
2. **Core starvation.** The audio task spun on `gen->loop()`, which returns
   immediately once the I2S DMA is full, starving the prio-1 `loopTask` pinned to
   the same core. Web UI and serial CLI died during playback. One `vTaskDelay(1)`
   per pass fixes it.
3. **`Transfer-Encoding: chunked` was never handled.** nginx-fronted stations
   chunk HTTP/1.1 bodies; the chunk framing went to the decoder and shifted the
   ICY metaint, desyncing metadata permanently. Only visible on a station that
   sets the MP3 CRC bit. Fixed with a resumable dechunker (`NetStream::rawRead`).
   Note `curl` hides this by negotiating HTTP/2 — use `curl --http1.1`.
4. **`AudioFileSourceBuffer` is unusable for live streams** — it refills with one
   blocking full-buffer read a realtime-paced station can never satisfy. Replaced
   with a producer task + FreeRTOS stream buffer in PSRAM.
5. **Auto-resume bricked the device** when the saved station crashed the decoder.
   Guarded with an NVS boot counter that forgets the station after 3 unstable starts.
6. **Reconnect only retried while the decoder lived**, so any outage longer than
   one retry was permanent silence. Now a retry state machine that survives
   teardown and waits for `WL_CONNECTED`. Verified against a forced 60 s outage.
7. **Unknown codecs fell back to MP3** and libmad does not survive Ogg pages.
   Now explicitly refused with a reason.

## Open problems, ranked

### 1. HLS (`.m3u8`) — 10% of the directory, biggest coverage gap
Scoped, not started. `schreibfaul1/ESP32-audioI2S` implements it inside an
8399-line file and needs: playlist parsing (`EXT-X-MEDIA-SEQUENCE`,
`EXT-X-TARGETDURATION`), **MPEG-TS demuxing** (188-byte packets, `0x47` sync,
PID filter, PES extraction), and AES-128 for some streams. Elektor's ESP32-S3 AM
transmitter polls only the **first 4 KB** of the playlist to check the sequence
number rather than refetching it whole.

**Questions for research:** Is there a smaller standalone HLS/TS demux worth
vendoring rather than reimplementing? Do enough HLS stations serve plain ADTS
AAC segments (no TS container) that a partial implementation covers most of the
10%? How do others handle the rolling-window refetch cadence without drift?

### 2. `netfill` core affinity — attempted, INCONCLUSIVE
The build now has `-DNETFILL_CORE` (0 = wifi core, 1 = alongside the decoder) so
this is a one-flag experiment. Against the LAN source: core 1 gave 47.0 KB/s
(tight, 46.7–47.6) and core 0 gave 40.3 KB/s (wide, 28.9–47.6) — suggestive, and
the tighter variance is the more interesting half. **But the repeat run collapsed
to 0.7 KB/s and RSSI was found to have drifted to −78/−88 dBm, so none of it is
trustworthy.** Redo it with the puck at a known, stable distance from the AP,
sampling RSSI before and after each run and discarding any run where it moves
more than a few dB. Currently shipped on core 1 as the better guess, not a
proven win.

**Questions:** What throughput do comparable S3 radios achieve on a LAN? Is the
limit lwIP window sizing (`CONFIG_LWIP_TCP_WND_DEFAULT`, `RECVMBOX_SIZE`), and
are those reachable from an Arduino build without a custom sdkconfig? Does
AP+STA coexistence cost measurably once RSSI is controlled (an earlier +6%
reading has the same drift problem)?

### 3. HE-AAC buffer overrun — SOLVED

**Root cause.** ESP8266Audio's `AudioGeneratorAAC` sizes its PCM output buffer
for plain AAC:

```c
outSample = (int16_t *)malloc(1024 * 2 * sizeof(uint16_t));   // 4096 bytes
```

The bundled libhelix is built with `AAC_ENABLE_SBR`, and with SBR active a frame
produces twice as many samples per channel (`aacdec.c:186`:
`outputSamps = nChans * AAC_MAX_NSAMPS * (sbrEnabled ? 2 : 1)`). A stereo HE-AAC
frame therefore writes `2 * 2048 * 2 = 8192` bytes into that 4096-byte buffer:
**a 4096-byte overrun on every frame**, on every `audio/aacp` station.

**Why it looked like an HTTPS bug.** The overrun always happens. Whether it is
fatal depends only on what the allocator placed after the buffer, and mbedTLS's
allocations shift the heap enough to put the IDLE0 task control block in the
landing zone. Destroying a TCB kills the device. Plain HTTP looked clean because
the control stations were LC-AAC and MP3 — never `audio/aacp`. Codec, not
transport, was the variable the whole time.

**The evidence.** A core dump of the crashed task shows 16-bit stores at a
4-byte stride running through IDLE0's TCB — interleaved stereo, channel 0, which
is exactly `outbuf[i * nChans + ch]`. The high half of each 32-bit word survives,
including `0x454C` in the name field: the "LE" of "IDLE0".

**The fix.** `outSample` is `protected`, so `SbrSafeAAC` in `main.cpp` subclasses
the generator and re-sizes the buffer to the real worst case (2048 samples per
channel, 2 channels) instead of forking the library. Costs 4 KB of heap.

**Worth reporting upstream** — this affects any ESP8266Audio user playing HE-AAC,
which is much of internet radio.

**The boot guard held.** A device that had saved an HE-AAC station did reboot
repeatedly, but the guard clears `bootTry` only after 20 s of `STREAMING` and the
crash lands at ~6 s, so the counter advanced and the station was forgotten after
three starts. Prolonged looping seen on the bench was the test harness re-tuning
the crashing station on every trial, not a guard failure.

### 4. Codec coverage
FLAC and Opus decoders are bundled in ESP8266Audio and currently refused by the
guard — enabling them is cheap but each needs testing given (3). **OGG Vorbis has
no decoder** and would mean vendoring tremor; at ~1% of stations, likely not
worth it. Confirm whether any lightweight Vorbis decoder is realistic on S3.

### 5. Not yet done
- 24-hour soak (longest so far: 25 minutes, clean).
- Package as merged `factory.bin` at `0x0` for the scriptkitty flasher.
- Graceful degradation: refuse or warn on stations above the measured ceiling
  rather than serving a stutter (Ka-Radio32 does something similar — software MP3
  everywhere, AAC only where the hardware sustains it).

## Bench notes that will save you hours

- **Every `esptool` operation re-arms `FORCE_DOWNLOAD_BOOT` on these pucks.**
  Symptom: port present, esptool connects instantly, app prints nothing. Clear
  `0x6000812C` with `--before default-reset --after watchdog-reset`, then confirm
  the app really runs by checking that `--before no-reset chip-id` **fails**.
- **Flashing the merged factory bin at `0x0` wipes NVS** (esptool pads the gaps
  with `0xFF` and NVS lives at `0x9000`), so Wi-Fi credentials vanish. Iterate
  with app-only at `0x10000`; use the full image only for a virgin board.
- **Constant baud.** A `uploadfs` at 921600 died 77% in; the same write at a
  constant 115200 completed and still ran at 1890 kbit/s. It's the mid-flash baud
  renegotiation that breaks, not the speed.
- The firmware has a **serial CLI** (`help`, `status`, `stats`, `tasks`, `tune`,
  `search`, `icy 0|1`, `ap 0|1`, `netkill <s>`). `tasks` dumps per-task state and
  stack watermark and is the fastest way to tell "blocked" from "dead" from
  "about to overflow". `netkill` forces a Wi-Fi outage to test ride-through.
- `icy-br` lies. One station advertises 128 kbps and serves 64. Decode the first
  frame header for the truth.
