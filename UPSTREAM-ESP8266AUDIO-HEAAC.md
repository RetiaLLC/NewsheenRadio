# Upstream report: AudioGeneratorAAC overruns its PCM buffer on HE-AAC

Draft for <https://github.com/earlephilhower/ESP8266Audio>. Everything below was
reproduced and fixed on ESP32-S3 hardware.

## Summary

`AudioGeneratorAAC` allocates its PCM output buffer for 1024 samples per
channel, but the bundled libhelix-aac is compiled with SBR enabled, and an SBR
frame decodes to 2048 samples per channel. A stereo HE-AAC stream therefore
writes 8192 bytes into a 4096-byte heap buffer, on every frame.

HE-AAC (`audio/aacp`) is common in internet radio, so this is easy to hit.

## The two halves

`AudioGeneratorAAC.cpp`, both `begin()` and the preallocating variant:

```c
outSample = (int16_t *)malloc(1024 * 2 * sizeof(uint16_t));   // 4096 bytes
```

`libhelix-aac/aacdec.c`:

```c
aacFrameInfo->outputSamps = aacDecInfo->nChans * AAC_MAX_NSAMPS *
                            (aacDecInfo->sbrEnabled ? 2 : 1);
```

and `aaccommon.h` has `#define AAC_ENABLE_SBR 1`. The header comment in
`aacdec.c` states it plainly: *"number of output samples = 1024 per channel
(2048 if SBR enabled)"*.

So for stereo with SBR: `2 * 1024 * 2 = 4096` samples, `8192` bytes, into a
buffer sized `4096`.

## Why it is easy to misdiagnose

The overrun is silent until the allocator happens to place something important
immediately after `outSample`. In our case that was a FreeRTOS task control
block, and the failure presented as three different panics from one build:

```
***ERROR*** A stack overflow in task IDLE0 has been detected.
Guru Meditation Error: Core 0 panic'ed (LoadProhibited)   [in xTaskIncrementTick]
Guru Meditation Error: Core 0 panic'ed (Unhandled debug exception)  [BREAK instr]
```

About half the time the reported task name was garbage, because the corruption
reached the name field in the TCB.

It also looked transport-dependent for a long time: HTTPS stations crashed and
plain HTTP ones did not. That was a coincidence of heap layout — mbedTLS's
allocations moved the task control block into the landing zone. Our plain-HTTP
control stations were all LC-AAC or MP3, never `audio/aacp`.

A core dump identified it: 16-bit stores at a 4-byte stride running through the
TCB, with the high half of every 32-bit word left intact. That is one channel of
an interleaved stereo frame, `outbuf[i * nChans + ch]` — a PCM buffer overrun,
not a stack overflow.

## Suggested fix

Size the buffer for the worst case the bundled decoder can actually produce:

```c
outSample = (int16_t *)malloc(2048 * 2 * sizeof(int16_t));   // 8192 bytes
```

That costs 4 KB of heap on every AAC instance. If that is unacceptable for
smaller targets, the allocation could key off `AAC_ENABLE_SBR`, or
`AudioGeneratorAAC::loop()` could check `fi.outputSamps` against the buffer
capacity and refuse the frame rather than write past the end.

The preallocating `begin(void *space, int size)` path needs the same change, and
its space calculation updating to match.

## Workaround for users

`outSample` is `protected`, so a subclass can re-size it without patching the
library:

```cpp
class SbrSafeAAC : public AudioGeneratorAAC {
public:
    bool begin(AudioFileSource *src, AudioOutput *out) override {
        if (!AudioGeneratorAAC::begin(src, out)) return false;
        free(outSample);
        outSample = (int16_t *)malloc(2048 * 2 * sizeof(int16_t));
        return outSample != nullptr;
    }
};
```

## Verification

Two ESP32-S3 units, same bench, same stations, same window, tuning
`https://smoothjazz.cdnstream1.com/2585_64.aac` (`audio/aacp`):

| Unit | Build | Result |
| --- | --- | --- |
| A | unfixed | 4/4 crashed, median 5.8 s |
| A | fixed | 0/7 crashed |
| B | fixed | 0/10 crashed |

Followed by a multi-station soak over HE-AAC, LC-AAC and MP3 with no failures
and flat heap.
