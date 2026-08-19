// A small WLED-style effect engine for the puck's 8-pixel ring.
//
// These are original implementations of classic patterns, not WLED code — the
// radio firmware is not WLED and does not link any of it. What carries over is
// the shape of the controls people expect: an effect, a primary colour, a speed
// and a brightness, all live-adjustable.
//
// render() is called from the LED task at a fixed rate and is pure: it reads
// only its parameters and the elapsed time, so it can be swapped mid-frame
// without any state to unwind.

#pragma once
#include <Arduino.h>
#include <Adafruit_NeoPixel.h>

enum EffectId {
    FX_SOLID = 0,
    FX_BREATHE,
    FX_RAINBOW,
    FX_RAINBOW_RING,
    FX_COLORLOOP,
    FX_CHASE,
    FX_THEATER,
    FX_COMET,
    FX_TWINKLE,
    FX_FIRE,
    FX_VU,              // level meter with peak-hold
    FX_SPECTRUM,        // three bands spread around the ring
    FX_PULSE,           // beat-triggered bloom
    FX_AURORA,          // slow colour field breathed by mid energy
    FX_SPARKLE,         // treble strikes
    FX_COUNT
};

static const char *const EFFECT_NAMES[] = {
    "Solid", "Breathe", "Rainbow", "Rainbow Ring", "Colorloop", "Chase",
    "Theater", "Comet", "Twinkle", "Fire", "Audio VU",
    "Spectrum", "Pulse", "Aurora", "Sparkle"
};
static_assert(sizeof(EFFECT_NAMES) / sizeof(EFFECT_NAMES[0]) == FX_COUNT,
              "effect name table out of sync with EffectId");

struct EffectState {
    uint8_t effect = FX_RAINBOW_RING;
    uint8_t r = 255, g = 120, b = 40;   // primary colour (warm, matches the lamp)
    uint8_t brightness = 140;
    uint8_t speed = 128;                // 0 = crawl, 255 = frantic
    float audioLevel = 0.0f;            // 0..1, fed from the audio tap
    float bass = 0.0f, mid = 0.0f, treble = 0.0f;   // AGC-normalised band energy
    uint32_t beatAt = 0;                // millis() of the last detected beat
};

class EffectEngine {
public:
    EffectEngine(Adafruit_NeoPixel &strip, uint16_t count) : strip(strip), n(count) {}

    void render(const EffectState &s, uint32_t nowMs) {
        // Speed maps to a phase that advances between ~0.15x and ~4x real time.
        float rate = 0.15f + (s.speed / 255.0f) * 3.85f;
        phase += (nowMs - lastMs) * rate;
        lastMs = nowMs;
        uint32_t t = (uint32_t)phase;

        strip.setBrightness(s.brightness);
        switch (s.effect) {
            case FX_SOLID:        solid(s);            break;
            case FX_BREATHE:      breathe(s, t);       break;
            case FX_RAINBOW:      rainbow(t);          break;
            case FX_RAINBOW_RING: rainbowRing(t);      break;
            case FX_COLORLOOP:    colorloop(t);        break;
            case FX_CHASE:        chase(s, t);         break;
            case FX_THEATER:      theater(s, t);       break;
            case FX_COMET:        comet(s, t);         break;
            case FX_TWINKLE:      twinkle(s, t);       break;
            case FX_FIRE:         fire(t);             break;
            case FX_VU:           vu(s);               break;
            case FX_SPECTRUM:     spectrum(s);         break;
            case FX_PULSE:        pulse(s, nowMs);     break;
            case FX_AURORA:       aurora(s, t);        break;
            case FX_SPARKLE:      sparkle(s, t, nowMs); break;
            default:              solid(s);            break;
        }
        strip.show();
    }

private:
    // ------------------------------------------------------------- primitives
    uint32_t scaled(const EffectState &s, float f) const {
        f = constrain(f, 0.0f, 1.0f);
        return strip.Color(s.r * f, s.g * f, s.b * f);
    }

    // 8-bit triangle wave — cheaper than sin() and plenty for LED work.
    static uint8_t tri(uint8_t x) {
        return (x < 128) ? (uint8_t)(x * 2) : (uint8_t)((255 - x) * 2);
    }

    // ---------------------------------------------------------------- effects
    void solid(const EffectState &s) {
        for (uint16_t i = 0; i < n; i++) {
            strip.setPixelColor(i, strip.Color(s.r, s.g, s.b));
        }
    }

    void breathe(const EffectState &s, uint32_t t) {
        float f = 0.08f + 0.92f * (tri((t / 8) & 0xFF) / 255.0f);
        for (uint16_t i = 0; i < n; i++) {
            strip.setPixelColor(i, scaled(s, f));
        }
    }

    void rainbow(uint32_t t) {
        uint16_t hue = (t * 12) & 0xFFFF;
        uint32_t c = strip.gamma32(strip.ColorHSV(hue));
        for (uint16_t i = 0; i < n; i++) {
            strip.setPixelColor(i, c);
        }
    }

    void rainbowRing(uint32_t t) {
        for (uint16_t i = 0; i < n; i++) {
            uint16_t hue = ((t * 12) + (uint32_t)i * 65536UL / n) & 0xFFFF;
            strip.setPixelColor(i, strip.gamma32(strip.ColorHSV(hue)));
        }
    }

    void colorloop(uint32_t t) {
        // Slow drift with the whole ring holding a narrow slice of the wheel.
        for (uint16_t i = 0; i < n; i++) {
            uint16_t hue = ((t * 4) + (uint32_t)i * 2000UL) & 0xFFFF;
            strip.setPixelColor(i, strip.gamma32(strip.ColorHSV(hue, 255, 255)));
        }
    }

    void chase(const EffectState &s, uint32_t t) {
        uint16_t head = (t / 90) % n;
        for (uint16_t i = 0; i < n; i++) {
            strip.setPixelColor(i, (i == head) ? strip.Color(s.r, s.g, s.b) : scaled(s, 0.05f));
        }
    }

    void theater(const EffectState &s, uint32_t t) {
        uint8_t offset = (t / 120) % 3;
        for (uint16_t i = 0; i < n; i++) {
            strip.setPixelColor(i, (i % 3 == offset) ? strip.Color(s.r, s.g, s.b) : 0);
        }
    }

    void comet(const EffectState &s, uint32_t t) {
        // Fractional head position so the tail slides smoothly around the ring.
        float head = fmodf(t / 90.0f, (float)n);
        for (uint16_t i = 0; i < n; i++) {
            float d = head - i;
            if (d < 0) {
                d += n;                 // wrap: the tail follows across the seam
            }
            strip.setPixelColor(i, scaled(s, powf(0.45f, d)));
        }
    }

    void twinkle(const EffectState &s, uint32_t t) {
        uint32_t step = t / 120;
        for (uint16_t i = 0; i < n; i++) {
            // Hash of (pixel, step) — deterministic sparkle with no RNG state.
            uint32_t h = (i * 2654435761UL) ^ (step * 40503UL);
            h ^= h >> 13;
            float f = ((h & 0xFF) / 255.0f);
            strip.setPixelColor(i, scaled(s, f * f * f));   // cubed = sparse, punchy
        }
    }

    void fire(uint32_t t) {
        for (uint16_t i = 0; i < n; i++) {
            uint32_t h = (i * 374761393UL) ^ ((t / 60) * 668265263UL);
            h ^= h >> 15;
            uint8_t heat = 110 + (h & 0x7F);
            // Ember palette: red rises to orange, never reaching blue.
            strip.setPixelColor(i, strip.Color(heat, (heat * heat) >> 10, 0));
        }
    }

    // --------------------------------------------------------- audio-reactive
    // Three bands laid around the ring: bass at the bottom, treble at the top.
    // Each pixel interpolates between its neighbours' bands so the ring reads as
    // a continuous spectrum instead of three blocks.
    void spectrum(const EffectState &s) {
        const float band[3] = {s.bass, s.mid, s.treble};
        const uint8_t col[3][3] = {{255, 40, 0}, {255, 200, 0}, {80, 180, 255}};
        for (uint16_t i = 0; i < n; i++) {
            float f = (float)i / (n - 1) * 2.0f;      // 0..2 across the bands
            int lo = (int)f;
            if (lo > 1) {
                lo = 1;
            }
            float frac = f - lo;
            float e = band[lo] * (1 - frac) + band[lo + 1] * frac;
            e = e * e;                                // square: quiet stays dark
            strip.setPixelColor(i, strip.Color(
                (col[lo][0] * (1 - frac) + col[lo + 1][0] * frac) * e,
                (col[lo][1] * (1 - frac) + col[lo + 1][1] * frac) * e,
                (col[lo][2] * (1 - frac) + col[lo + 1][2] * frac) * e));
        }
    }

    // A bloom on each detected beat, decaying over ~350 ms, tinted by the
    // treble content so a hi-hat-heavy track looks different from a kick-only one.
    void pulse(const EffectState &s, uint32_t now) {
        uint32_t age = now - s.beatAt;
        float b = s.beatAt ? expf(-(float)age / 160.0f) : 0.0f;
        float base = 0.06f + s.audioLevel * 0.18f;
        for (uint16_t i = 0; i < n; i++) {
            float e = base + b * (0.55f + 0.45f * s.bass);
            if (e > 1.0f) {
                e = 1.0f;
            }
            uint8_t r = (s.r * (1 - s.treble) + 255 * s.treble) * e;
            uint8_t g = (s.g * (1 - s.treble) + 255 * s.treble) * e;
            strip.setPixelColor(i, strip.Color(r, g, s.b * e));
        }
    }

    // Two counter-rotating colour waves, breathing with mid energy. Deliberately
    // slow — this is the one to leave on in a room rather than stare at.
    void aurora(const EffectState &s, uint32_t t) {
        for (uint16_t i = 0; i < n; i++) {
            float a = (float)i / n * TWO_PI;
            float w1 = sinf(a + t / 900.0f);
            float w2 = sinf(a * 2.0f - t / 1400.0f);
            float m = 0.5f + 0.5f * (w1 * 0.6f + w2 * 0.4f);
            uint16_t hue = (uint16_t)((t * 3 + m * 14000) + s.mid * 9000) & 0xFFFF;
            uint8_t v = 38 + 217 * m * (0.35f + 0.65f * s.audioLevel);
            strip.setPixelColor(i, strip.gamma32(strip.ColorHSV(hue, 220, v)));
        }
    }

    // Treble strikes light single pixels that then fade. Percussion and vocal
    // sibilance show up here in a way the level meter cannot express.
    void sparkle(const EffectState &s, uint32_t t, uint32_t now) {
        for (uint16_t i = 0; i < n; i++) {
            spark[i] *= 0.86f;
        }
        if (s.treble > 0.22f) {
            uint32_t h = (now * 2654435761UL) ^ (t * 40503UL);
            h ^= h >> 13;
            uint16_t idx = h % n;
            float add = s.treble * s.treble;
            if (spark[idx] < add) {
                spark[idx] = add;
            }
        }
        for (uint16_t i = 0; i < n; i++) {
            float e = spark[i];
            uint8_t warm = 22 * (0.3f + 0.7f * s.audioLevel);
            strip.setPixelColor(i, strip.Color(
                s.r * e + warm, s.g * e + warm * 0.7f, s.b * e + warm * 0.4f));
        }
    }

    void vu(const EffectState &s) {
        // Peak-hold: the classic meter behaviour. The dot marks recent maximum
        // and falls slowly, which makes dynamics legible in a way a bare bar is not.
        if (s.audioLevel > peak) {
            peak = s.audioLevel;
            peakHold = 28;
        } else if (peakHold) {
            peakHold--;
        } else {
            peak -= 0.012f;
            if (peak < 0) {
                peak = 0;
            }
        }
        float lit = s.audioLevel * n;
        for (uint16_t i = 0; i < n; i++) {
            float frac = constrain(lit - i, 0.0f, 1.0f);
            uint8_t v = 255 * frac;
            if (i >= n - 2) {
                strip.setPixelColor(i, strip.Color(v, 0, 0));
            } else if (i >= n - 4) {
                strip.setPixelColor(i, strip.Color(v, v * 0.6f, 0));
            } else {
                strip.setPixelColor(i, strip.Color(v * 0.2f, v, 0));
            }
        }
    }

    float spark[16] = {0};
    float peak = 0;
    int peakHold = 0;
    Adafruit_NeoPixel &strip;
    uint16_t n;
    float phase = 0;
    uint32_t lastMs = 0;
};
