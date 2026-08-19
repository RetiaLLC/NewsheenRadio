# Test plan: the HTTPS crash

> **RESOLVED 2026-08-19.** It was never an HTTPS bug. `AudioGeneratorAAC` sized
> its PCM buffer for 1024 samples per channel while the SBR-enabled libhelix
> emits 2048, so every HE-AAC frame overran the buffer by 4096 bytes. TLS only
> changed the heap layout so the overrun landed on IDLE0's task control block.
> Fixed by `SbrSafeAAC` in `src/main.cpp`. See `RESEARCH-BRIEF.md` §3 and
> `UPSTREAM-ESP8266AUDIO-HEAAC.md`.
>
> **What actually cracked it,** for the next investigation:
>
> | Step | Verdict |
> | --- | --- |
> | Phase 0, measure the failure rate first | Essential. Moving the units gave a **deterministic** 5.8 s repro, which made everything after it cheap. |
> | T1.1 core dump | **Decisive.** The corruption pattern named the writer. |
> | T2.3-style ablation ladder | Useful. Ruled out the handshake in one run. |
> | T1.2 idle-stack instrumentation | Misleading on its own — IDLE0 sat at 232 bytes free right up to the panic, which looked like "not exhaustion" and was really "corrupted from outside". |
> | T3.x sdkconfig experiments | Never needed. The toolchain fight would have been wasted effort. |
>
> The plan below is kept as written, because the ordering held up.

## What this bug is

Tuning an HTTPS station panics the device at decoder start, right after the
stream prebuffers. It is intermittent. It can happen on the first tune from a
quiet device, with no prior stream to tear down. Plain HTTP is unaffected.

The panic presents in at least three forms from the same build:
`stack overflow in task IDLE0`, `LoadProhibited` inside `xTaskIncrementTick`,
and `BREAK instr` inside `_UserExceptionVector`. All carry a `|<-CORRUPTED`
backtrace, and the reported task name is garbage about half the time. Treat
them as one defect.

## What counts as fixed

All four, not any one:

1.  The mechanism is explained, not just made to go away.
2.  500 consecutive HTTPS tunes across both units with no panic.
3.  A 24-hour rotation soak over HTTPS stations with no panic.
4.  The plain-HTTP control still passes, so the fix did not trade one failure
    for another.

A build that merely survives longer is not fixed. Two earlier "fixes" — raising
the audio task stack and reworking the producer lifecycle — each looked like
they worked and did not.

## Before you start

Bench state as of 2026-08-19:

| Unit | Port | IP | RSSI |
| --- | --- | --- | --- |
| Puck A | `/dev/ttyACM0` | 192.168.1.221 | −85 dBm |
| Puck B | `/dev/ttyACM1` | 192.168.1.52 | −83 dBm |

Both on `SpectrumSetup-44`. Scripts live in `~/nsr/` on workbench5 and in
`tools/` in this repo.

Two cautions:

*   **Both links are weak.** −83 to −85 dBm means retransmissions are common,
    and that is an uncontrolled variable in every test below. Phase 4 addresses
    it; until then, do not compare runs taken at different distances.
*   **Verify every flash.** Check for `Hash of data verified`. Piping esptool
    through `tail` hides its exit status, and a silently failed flash looks
    exactly like a build whose change did nothing.

---

## Phase 0: establish the failure rate

Everything downstream is a comparison, and the fault is intermittent. Without a
baseline you cannot tell a fix from luck. **Do this first.**

### T0.1 Tunes-to-failure, 20 trials per unit

Boot, tune one HTTPS station, record whether it panics and after how long.
Repeat 20 times per unit. Report median and range of tunes-to-failure, not just
a pass rate.

```bash
python3 tools/singletls.py https://smoothjazz.cdnstream1.com/2585_64.aac 90
```

Success criterion for later phases: a change is only interesting if it moves
the failure rate outside the range this establishes.

### T0.2 Same, plain HTTP

The negative control. 30 changes were clean once; confirm that holds across 20
trials on both units so it is a real baseline rather than one lucky run.

```bash
python3 tools/plaintest.py
```

---

## Phase 1: get direct evidence

The fastest route to the answer. Stop inferring from panic text.

### T1.1 Make the core dump work — highest value single test

Every panic so far ends with `esp_core_dump_flash: Not enough space to save
core dump!`, so the one artifact that would name the faulting task and show its
stack is being thrown away.

1.  Add or enlarge a `coredump` partition in the partition CSV (64 KB is
    plenty).
2.  Confirm it works before relying on it, with a deliberate `abort()`.
3.  Reproduce, then read it back:

    ```bash
    esptool -p <port> read-flash <coredump_offset> <size> core.bin
    espcoredump.py info_corefile -c core.bin -t raw .pio/build/newsheen-speaker/firmware.elf
    ```

This gives the real faulting task, its stack, and every task's state at the
moment of death. Expect it to answer the question outright.

### T1.2 Instrument IDLE stack headroom around a TLS tune

Cheap, and it confirms or kills the leading suspicion without a toolchain
fight. IDLE0 runs at roughly 77% of its 1024-byte stack at rest.

Add a sampler task at high priority that reads
`uxTaskGetStackHighWaterMark()` for both idle tasks every few milliseconds
through a TLS tune, and logs the minimum. If it marches toward zero during the
handshake, the idle stack is confirmed as the victim.

### T1.3 Paint the idle stack

If T1.2 is too coarse, fill IDLE0's stack with a known pattern at boot and
measure how far it has been consumed after a TLS tune. This catches a single
deep excursion that periodic sampling can miss.

---

## Phase 2: shrink the system

Cut away everything that is not required to reproduce. This is what turns a
whole-firmware mystery into a small one.

### T2.1 Minimal reproducer firmware — highest value after the core dump

Build the smallest program that still crashes: Wi-Fi join, then a loop of
`WiFiClientSecure` connect, GET, read a few KB, close. No audio, no I2S, no
LEDs, no web server, no mDNS, no PSRAM ring.

*   **If it still panics**, the bug is in platform TLS usage, not in this
    firmware, and the whole investigation moves to the ESP-IDF and mbedTLS
    layer. It also becomes trivially reportable upstream.
*   **If it does not**, add subsystems back one at a time until it does. The one
    that reintroduces it is the interaction to study.

### T2.2 Serve HTTPS from the Pi

Point the device at a local TLS server instead of internet stations. Removes
station variance, upstream redirects, ICY quirks, and most RF distance.

```bash
openssl req -x509 -newkey rsa:2048 -nodes -keyout k.pem -out c.pem -days 30 -subj "/CN=192.168.1.17"
python3 -c "import http.server,ssl,functools; ..."   # serve a large .mp3 over TLS
```

Confirm the crash still occurs. If it does, every later test gets faster and
repeatable. If it does not, the trigger involves something about real stations
and that is itself a strong clue.

### T2.3 Component ablation

One build flag each, reproducing after every change. Disable in turn: the LED
task, the visualizer feature extractor, the web server, mDNS, the captive
portal DNS server. Note which, if any, changes the failure rate.

---

## Phase 3: test the two mechanism hypotheses

Both need `custom_sdkconfig`, which rebuilds the IDF libraries. It wedged twice
at "Reading CMake configuration" and once failed with
`fatal: not a git repository` from its own scaffolding. **Budget real time and
fix the toolchain first** — see the appendix.

### T3.1 Raise the idle task stack

```ini
custom_sdkconfig =
  CONFIG_FREERTOS_IDLE_TASK_STACKSIZE=4096
```

Arduino ships 1024 here, below ESP-IDF's own 1536 default. If the reproducer
goes quiet, confirm it is a real fix and not a timing shift by re-running the
full Phase 0 trial count — this is exactly how the audio-stack theory fooled us.

### T3.2 Disable hardware crypto

```ini
custom_sdkconfig =
  CONFIG_MBEDTLS_HARDWARE_AES=n
  CONFIG_MBEDTLS_HARDWARE_MPI=n
  CONFIG_MBEDTLS_HARDWARE_SHA=n
```

All three are enabled today, and they are the clearest thing that differs
between the HTTPS and plain-HTTP paths. Software crypto is slower but proves
whether the accelerators are involved.

If this fixes it, narrow to a single accelerator by re-enabling them one at a
time. MPI is the first suspect: it is used for RSA key exchange and is
DMA-capable.

### T3.3 Stream ring in internal RAM

The ring lives in PSRAM. Rebuild with it in internal RAM (shrink it to fit) to
test any DMA-versus-PSRAM interaction. Cheap, and it needs no sdkconfig.

---

## Phase 4: environment controls

Run these before concluding anything from a fix, because they can each change
the failure rate on their own.

### T4.1 Signal strength

Repeat T0.1 with a unit close to the access point, at roughly −40 dBm. Sample
RSSI before and after every run and discard runs that move more than a few dB.
If the failure rate collapses at strong signal, retransmission behavior is part
of the mechanism, and every earlier comparison needs re-reading.

### T4.2 Second unit

Reproduce on Puck B. Confirms this is not one bad module. Then run both
concurrently to check whether contention changes the rate.

### T4.3 Different network

A phone hotspot, ideally WPA2 and 2.4 GHz. Rules out this specific access
point, its channel, and its DHCP behavior.

### T4.4 Certificate type

Tune stations with RSA versus ECDSA certificates. If only RSA crashes, that
points hard at hardware MPI and connects Phase 3.2 to a real-world trigger.

---

## Phase 5: confirm the fix

Only run these once a mechanism is explained.

### T5.1 500 consecutive HTTPS tunes per unit

At the failure rate measured in T0.1, 500 clean tunes is strong evidence.

### T5.2 24-hour rotation soak

Rotate HTTPS stations every few minutes for 24 hours on both units. Watch heap
free across the run: a slow leak across TLS sessions would show here and is
worth catching regardless.

### T5.3 Plain-HTTP regression

Re-run T0.2. Confirm the fix did not break the path that always worked.

### T5.4 Re-verify the release

If the fix ships, flash the actual release asset — not a local build — and
re-run T5.1 before promoting. The CI artifact never matches a local build
byte-for-byte.

---

## Appendix: toolchain notes

**`custom_sdkconfig` on pioarduino.** Supported, contrary to what the research
brief previously assumed. It rebuilds the IDF libraries, takes several minutes,
and writes `.dummy/`, `CMakeLists.txt`, `dependencies.lock`,
`managed_components/` and `sdkconfig.defaults` into the project directory.
Delete those when reverting. Two of three attempts wedged at "Reading CMake
configuration" with cmake at 0% CPU; killing and retrying got further. Resolve
this before Phase 3 rather than during it.

**Reading serial.** Hold DTR de-asserted (`dtr=False, rts=False`) before
opening, or opening the port resets the board. Pulse RTS to reset deliberately.

**Flashing.** Use a constant 115200 baud and check for `Hash of data verified`.
A full image at `0x0` wipes NVS and with it the Wi-Fi credentials; flash the app
at `0x10000` while iterating.
