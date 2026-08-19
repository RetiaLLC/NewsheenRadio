// A tiny two-voice chiptune synth that plugs into ESP8266Audio as a generator.
//
// Square-wave lead + triangle bass, each running its own note list, mixed to
// 16-bit stereo at 22050 Hz — the same rate SAM speaks at, so the greeting and
// the song can share one I2S configuration with no reconfigure in between.
//
// loop() is non-blocking: it pushes samples until the I2S DMA ring is full, then
// returns, exactly like the MP3 generator. That keeps the button and the web
// server responsive while the puck is singing.

#pragma once
#include <Arduino.h>
#include <AudioGenerator.h>
#include <AudioOutput.h>

#define CHIP_RATE 22050

// Equal-tempered note table, in Hz. R = rest.
#define R    0
#define A3  220
#define B3  247
#define C4  262
#define D4  294
#define E4  330
#define F4  349
#define G4  392
#define A4  440
#define B4  494
#define C5  523
#define D5  587
#define E5  659
#define F5  698
#define G5  784
#define A5  880
#define B5  988
#define C6 1047

struct ChipNote {
    uint16_t freq;
    uint16_t ms;
};

// "Newsheen's Little Song" — an original bouncy pentatonic ditty in C major.
// 16 bars at ~150 bpm, roughly 13 seconds.
static const ChipNote SONG_LEAD[] = {
    // phrase A — hello!
    {E5,200}, {G5,200}, {A5,400}, {G5,200}, {E5,200}, {D5,400}, {R,100},
    {C5,200}, {D5,200}, {E5,400}, {D5,200}, {C5,200}, {A4,400}, {R,200},
    // phrase B — up on tiptoe
    {E5,200}, {G5,200}, {A5,200}, {C6,200}, {B5,200}, {A5,200}, {G5,400}, {R,100},
    {E5,200}, {D5,200}, {C5,200}, {D5,200}, {E5,400}, {C5,600}, {R,300},
    // phrase C — a little strut
    {G4,150}, {A4,150}, {C5,150}, {D5,150}, {E5,300}, {D5,300},
    {C5,150}, {D5,150}, {E5,150}, {G5,150}, {A5,600}, {R,200},
    // phrase D — and settle down purring
    {A5,200}, {G5,200}, {E5,200}, {D5,200}, {C5,300}, {E5,300}, {G5,300},
    {C6,800}, {R,400},
};

static const ChipNote SONG_BASS[] = {
    {C4,800}, {A3,800}, {F4,800}, {G4,800},
    {C4,800}, {A3,800}, {F4,800}, {G4,800},
    {C4,800}, {F4,800}, {C4,800}, {G4,800},
    {A3,800}, {F4,800}, {G4,800}, {C4,1200}, {R,400},
};

class AudioGeneratorChiptune : public AudioGenerator {
public:
    AudioGeneratorChiptune() {
        running = false;
    }

    // source is ignored — the notes live in flash.
    bool begin(AudioFileSource *source, AudioOutput *out) override {
        (void)source;
        if (!out) {
            return false;
        }
        output = out;
        output->SetRate(CHIP_RATE);
        output->SetChannels(2);      // ESP8266Audio 2.4 is always 16-bit
        if (!output->begin()) {
            return false;
        }
        leadIdx = bassIdx = 0;
        leadLeft = bassLeft = 0;
        leadFreq = bassFreq = 0;
        leadLen = bassLen = 0;
        leadPhase = bassPhase = 0;
        vibPhase = 0;
        sampleCount = 0;
        running = true;
        return true;
    }

    bool loop() override {
        if (!running) {
            return false;
        }
        // Push samples until I2S says it is full, then hand control back.
        for (int guard = 0; guard < 512; guard++) {
            if (leadLeft == 0 &&
                !advance(SONG_LEAD, LEAD_N, leadIdx, leadLeft, leadFreq, leadLen)) {
                // Lead finished: let the bass ring out, then stop.
                if (bassLeft == 0 && bassIdx >= BASS_N) {
                    stop();
                    return false;
                }
            }
            if (bassLeft == 0) {
                advance(SONG_BASS, BASS_N, bassIdx, bassLeft, bassFreq, bassLen);
            }

            int16_t s = render();
            int16_t frame[2] = {s, s};
            if (!output->ConsumeSample(frame)) {
                return true;    // DMA full — come back next loop()
            }
            if (leadLeft) {
                leadLeft--;
            }
            if (bassLeft) {
                bassLeft--;
            }
            sampleCount++;
        }
        return true;
    }

    bool stop() override {
        if (running) {
            output->flush();
            output->stop();
        }
        running = false;
        return true;
    }

    bool isRunning() override {
        return running;
    }

private:
    static const size_t LEAD_N = sizeof(SONG_LEAD) / sizeof(SONG_LEAD[0]);
    static const size_t BASS_N = sizeof(SONG_BASS) / sizeof(SONG_BASS[0]);

    // Pull the next note off a list. Returns false once the list is exhausted.
    // Each voice keeps its own note length so the envelopes stay independent.
    bool advance(const ChipNote *list, size_t n, size_t &idx, uint32_t &left,
                 uint32_t &freq, uint32_t &len) {
        if (idx >= n) {
            freq = 0;
            return false;
        }
        freq = list[idx].freq;
        left = (uint32_t)list[idx].ms * CHIP_RATE / 1000;
        len = left;
        idx++;
        return true;
    }

    // Mix one mono sample.
    int16_t render() {
        int32_t mix = 0;

        if (leadFreq) {
            // Gentle 5 Hz vibrato so the square wave sounds less like a smoke alarm.
            vibPhase += (uint32_t)((5.0f * 65536.0f * 65536.0f) / CHIP_RATE);
            float vib = 1.0f + 0.006f * sinf((float)vibPhase / 4294967296.0f * TWO_PI);
            leadPhase += (uint32_t)((leadFreq * vib) * (4294967296.0f / CHIP_RATE));
            int16_t sq = (leadPhase & 0x80000000UL) ? 9000 : -9000;
            mix += (int32_t)(sq * envelope(leadLeft, leadLen));
        }

        if (bassFreq) {
            bassPhase += (uint32_t)(bassFreq * (4294967296.0f / CHIP_RATE) / 2.0f);  // one octave down
            // Triangle from the top 16 bits of the phase accumulator.
            uint16_t p = bassPhase >> 16;
            int32_t tri = (p < 32768) ? (int32_t)p - 16384 : 49151 - (int32_t)p;
            mix += (tri * 5000) / 16384;
        }

        if (mix > 32767) {
            mix = 32767;
        }
        if (mix < -32768) {
            mix = -32768;
        }
        return (int16_t)mix;
    }

    // Percussive pluck: fast attack, slow decay, short release at the note end.
    float envelope(uint32_t remaining, uint32_t noteLen) const {
        if (noteLen == 0) {
            return 0.0f;
        }
        uint32_t elapsed = noteLen - remaining;
        const uint32_t attack = CHIP_RATE / 400;    // 2.5 ms
        const uint32_t release = CHIP_RATE / 50;    // 20 ms
        float e = 1.0f;
        if (elapsed < attack) {
            e = (float)elapsed / attack;
        } else if (remaining < release) {
            e = (float)remaining / release;
        }
        return e * (0.55f + 0.45f * (float)remaining / noteLen);   // decay toward the tail
    }

    size_t leadIdx = 0, bassIdx = 0;
    uint32_t leadLeft = 0, bassLeft = 0;
    uint32_t leadFreq = 0, bassFreq = 0;
    uint32_t leadPhase = 0, bassPhase = 0, vibPhase = 0;
    uint32_t leadLen = 0, bassLen = 0;
    uint32_t sampleCount = 0;
};


// A soft continuous sine used as the volume-adjust indicator.
//
// Adjusting volume with nothing playing is adjusting blind — the ring shows a
// number but your ears get nothing, and "how loud is this actually going to be"
// is the only question that matters. So while the ramp runs on an idle device we
// emit a tone at the live gain. Pitch rises with the setting as well, because on
// a small speaker at low volume a quiet tone and a silent one are hard to tell
// apart, and the pitch still reads clearly.
//
// Frequency is read from a caller-owned volatile on every block, so the button
// handler can sweep it without stopping and restarting the generator.
class AudioGeneratorTone : public AudioGenerator {
public:
    explicit AudioGeneratorTone(volatile float *hzSource) : hz(hzSource) {}

    bool begin(AudioFileSource *, AudioOutput *out) override {
        if (!out) {
            return false;
        }
        output = out;
        output->SetRate(CHIP_RATE);
        output->SetChannels(2);
        if (!output->begin()) {
            return false;
        }
        phase = 0;
        fade = 0.0f;
        running = true;
        return true;
    }

    bool loop() override {
        if (!running) {
            return false;
        }
        for (int guard = 0; guard < 512; guard++) {
            float f = hz ? *hz : 440.0f;
            phase += (uint32_t)(f * (4294967296.0f / CHIP_RATE));
            // 12-bit sine from the phase accumulator, kept deliberately quiet:
            // this is a reference tone, not an alarm.
            float ang = (float)(phase >> 16) / 65536.0f * TWO_PI;
            if (fade < 1.0f) {
                fade += 0.0004f;        // ~50 ms ramp-in, avoids a click
            }
            int16_t v = (int16_t)(sinf(ang) * 5200.0f * fade);
            int16_t frame[2] = {v, v};
            if (!output->ConsumeSample(frame)) {
                return true;
            }
        }
        return true;
    }

    bool stop() override {
        if (running) {
            output->stop();
        }
        running = false;
        return true;
    }

    bool isRunning() override {
        return running;
    }

private:
    volatile float *hz;
    uint32_t phase = 0;
    float fade = 0.0f;
};
