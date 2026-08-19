// SPDX-License-Identifier: MIT
// newsheen_viz.h — audio-reactive visualizer engine for the Newsheen Radio
// 8× WS2812B ring under the sheen topper (heavy silicone diffusion).
//
// DESIGN DOCTRINE (why these look good diffused):
//   The topper is a spatial low-pass filter: per-pixel identity is gone; what
//   survives is total luminance, blended color, gradients, and slow angular
//   motion of 1–2 soft lobes. So every mode here is a CONTINUOUS LIGHT FIELD
//   f(angle, t) sampled at the 8 pixel angles — never discrete pixel logic.
//   Rules: analogous palettes with one accent (full-spectrum mixes mud to
//   grey-white under diffusion); asymmetric envelopes (fast attack / slow
//   release) so motion feels musical, not twitchy; a luminance floor (it's a
//   lamp — never fully dark); lobes σ >= ~0.35 rad; rotation <= ~0.6 rev/s.
//   Gamma 2.6 applied at the very end (essential for smooth diffused fades).
//
// Integration: feed decoded PCM to FeatureExtractor from an AudioOutput "tee"
// wrapped around AudioOutputI2S (accumulate per-sample, finalize per block of
// FEAT_BLOCK samples); LED task reads the Features snapshot and calls
// Visualizer::render() at 30–60 fps. ~zero heap; all state is in the objects.
//
// This header is the CANONICAL math — the HTML simulator ports it 1:1.
// Host-testable: no Arduino APIs. C++17.

#pragma once
#include <cstdint>
#include <cmath>
#include <cstring>

namespace nh {

// ============================================================ small helpers
static const float VIZ_PI = 3.14159265358979f;
static const float VIZ_TAU = 6.28318530717959f;

static inline float clampf(float x, float lo, float hi) {
  return x < lo ? lo : (x > hi ? hi : x);
}
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float fractf(float x) { return x - floorf(x); }

// wrapped angular distance, result in [0, PI]
static inline float angDist(float a, float b) {
  float d = fabsf(fractf((a - b) / VIZ_TAU) ); // 0..1
  if (d > 0.5f) d = 1.0f - d;
  return d * VIZ_TAU;
}
// gaussian lobe around the ring
static inline float lobe(float theta, float center, float sigma) {
  float d = angDist(theta, center);
  return expf(-(d * d) / (2.0f * sigma * sigma));
}
// smooth organic noise in [0,1] — three incommensurate sines
static inline float onoise(float t, float seed = 0.f) {
  float v = sinf(VIZ_TAU * (0.110f * t + seed)) +
            0.62f * sinf(VIZ_TAU * (0.233f * t + 1.7f + seed * 2.3f)) +
            0.39f * sinf(VIZ_TAU * (0.411f * t + 4.1f + seed * 3.1f));
  return 0.5f + v / (2.0f * 2.01f);
}
static inline float smoothstepf(float x) {
  x = clampf(x, 0.f, 1.f);
  return x * x * (3.f - 2.f * x);
}

struct RGBf { float r, g, b; };

// h in degrees [0,360), s,v in [0,1]
static inline RGBf hsv(float h, float s, float v) {
  h = fractf(h / 360.f) * 6.f;
  int i = (int)h;
  float f = h - i, p = v * (1 - s), q = v * (1 - s * f), u = v * (1 - s * (1 - f));
  switch (i % 6) {
    case 0: return {v, u, p};
    case 1: return {q, v, p};
    case 2: return {p, v, u};
    case 3: return {p, q, v};
    case 4: return {u, p, v};
    default: return {v, p, q};
  }
}
static inline void addRGB(RGBf& a, const RGBf& b, float w = 1.f) {
  a.r += b.r * w; a.g += b.g * w; a.b += b.b * w;
}
// soft clip keeps additive blends blooming to white gracefully instead of clipping hue
static inline float softclip(float x) { return 1.f - expf(-x * 1.2f); }

// shortest-arc hue lerp (degrees)
static inline float hueLerp(float a, float b, float t) {
  float d = fractf((b - a) / 360.f + 0.5f) - 0.5f;   // -0.5..0.5
  return a + d * 360.f * t;
}

// ======================================================== feature extraction
// Feed decoded PCM (mono-summed) sample by sample; snapshot per block.
static const int FEAT_BLOCK = 1024;        // ~23 ms @ 44.1k

struct Features {
  // all post-AGC, roughly 0..1
  float rms = 0;        // block RMS
  float envF = 0;       // fast envelope  (atk ~8 ms,  rel ~180 ms)
  float envS = 0;       // slow envelope  (atk ~250 ms, rel ~1.8 s)
  float bass = 0, mid = 0, treb = 0;      // band envelopes (fast-ish)
  float tilt = 0.5f;    // treb/(bass+treb): 0 = bassy, 1 = bright
  float beat = 0;       // bass-onset pulse, 1 at onset, exp decay ~250 ms
  float beatAge = 9e9f; // seconds since last bass onset
  uint32_t beatCount = 0;
  float glint = 0;      // treble-transient pulse, decay ~120 ms
  uint32_t glintCount = 0;
  float onsetRate = 0;  // smoothed onsets/sec
  float silence = 1;    // 1 = silent/idle, 0 = active (smoothed gate)
};

class FeatureExtractor {
public:
  void begin(float sampleRate = 44100.f) {
    reset();
    sr_ = sampleRate;
    // one-pole k = 1 - exp(-2*pi*fc/fs) at the REAL stream rate (44.1k or 48k)
    kA_ = 1.f - expf(-VIZ_TAU * 150.f / sr_);
    kB_ = 1.f - expf(-VIZ_TAU * 1200.f / sr_);
    kC_ = 1.f - expf(-VIZ_TAU * 5000.f / sr_);
  }
  void reset() { *this = FeatureExtractor(); }

  // s in [-1,1]; call per mono sample. Returns true when a block completed.
  bool pushSample(float s) {
    // --- filter bank (one-pole LPs; cheap, and identical in the JS port)
    lpA_ += kA_ * (s - lpA_);      // ~150 Hz
    lpB_ += kB_ * (s - lpB_);      // ~1.2 kHz
    lpC_ += kC_ * (s - lpC_);      // ~5 kHz
    float bassS = lpA_;
    float midS  = lpB_ - lpA_;
    float trebS = s - lpC_;
    accR_ += s * s; accB_ += bassS * bassS; accM_ += midS * midS; accT_ += trebS * trebS;
    if (++n_ < FEAT_BLOCK) return false;
    finalizeBlock();
    return true;
  }
  const Features& features() const { return f_; }

  // dt-decay the transient pulses from the render loop (call with frame dt)
  void frameDecay(float dt) {
    f_.beat *= expf(-dt / 0.25f);
    f_.glint *= expf(-dt / 0.12f);
    f_.beatAge += dt;
  }

private:
  void finalizeBlock() {
    float inv = 1.0f / (float)n_;
    float rms = sqrtf(accR_ * inv), bass = sqrtf(accB_ * inv),
          mid = sqrtf(accM_ * inv), treb = sqrtf(accT_ * inv);
    accR_ = accB_ = accM_ = accT_ = 0; n_ = 0;
    float dtB = FEAT_BLOCK / sr_;

    // --- AGC: normalize across stations (target 0.25 RMS, slow, clamped)
    bool active = rms > 1e-3f;
    if (active) {
      float err = 0.25f - rms * gain_;
      gain_ = clampf(gain_ + err * 0.6f * dtB, 0.5f, 16.f);
    }
    rms *= gain_; bass *= gain_; mid *= gain_; treb *= gain_;

    f_.rms = rms;
    env(f_.envF, rms, dtB, 0.008f, 0.180f);
    env(f_.envS, rms, dtB, 0.250f, 1.800f);
    env(f_.bass, bass, dtB, 0.010f, 0.150f);
    env(f_.mid,  mid,  dtB, 0.015f, 0.200f);
    env(f_.treb, treb, dtB, 0.008f, 0.120f);
    env(bassSlow_, bass, dtB, 0.400f, 1.200f);
    env(trebSlow_, treb, dtB, 0.300f, 1.000f);
    f_.tilt = f_.treb / (f_.bass + f_.treb + 1e-4f);
    env(f_.silence, active ? 0.f : 1.f, dtB, 1.2f, 0.4f);   // slow in, fastish out

    // --- bass onset (beat)
    tSinceBeat_ += dtB;
    float ob = f_.bass - 1.55f * bassSlow_;
    if (ob > 0.045f && tSinceBeat_ > 0.14f) {
      f_.beat = 1.f; f_.beatAge = 0.f; f_.beatCount++;
      tSinceBeat_ = 0.f; onsets_ += 1.f;
    }
    // --- treble transient (glint)
    tSinceGlint_ += dtB;
    float og = f_.treb - 1.45f * trebSlow_;
    if (og > 0.035f && tSinceGlint_ > 0.05f) {
      f_.glint = 1.f; f_.glintCount++; tSinceGlint_ = 0.f;
    }
    // onset rate (smoothed, ~3 s window)
    rateAcc_ += dtB;
    if (rateAcc_ >= 0.5f) {
      float inst = onsets_ / rateAcc_;
      f_.onsetRate += (inst - f_.onsetRate) * 0.30f;
      onsets_ = 0; rateAcc_ = 0;
    }
  }
  static void env(float& e, float x, float dt, float atk, float rel) {
    float tau = (x > e) ? atk : rel;
    e += (x - e) * (1.f - expf(-dt / tau));
  }

  float sr_ = 44100.f;
  // one-pole coefficients for 150 / 1200 / 5000 Hz at 44.1k (k = 1-exp(-2πfc/fs))
  float kA_ = 0.02114f, kB_ = 0.15683f, kC_ = 0.50953f;
  float lpA_ = 0, lpB_ = 0, lpC_ = 0;
  double accR_ = 0, accB_ = 0, accM_ = 0, accT_ = 0;
  int n_ = 0;
  float gain_ = 4.f;
  float bassSlow_ = 0, trebSlow_ = 0;
  float tSinceBeat_ = 9.f, tSinceGlint_ = 9.f, onsets_ = 0, rateAcc_ = 0;
  Features f_;
};

// =============================================================== visualizer
static const int VIZ_N = 8;

enum VizMode : uint8_t {
  VIZ_LANTERN = 0,   // warm hearth glow, organic flicker, swells with music
  VIZ_OCEAN,         // teal two-lobe breathing; onsets wash a wave around
  VIZ_HEARTBEAT,     // lub-dub on bass onsets; calm 55 BPM pulse when quiet
  VIZ_AURORA,        // three drifting curtains, band-driven, green/teal/violet
  VIZ_EMBER,         // bass hits ignite expanding warm blooms (fire palette)
  VIZ_PRISM,         // magenta<->cyan balance point slides with spectral tilt
  VIZ_GLINT,         // ice glints on treble transients over dim indigo
  VIZ_BEACON,        // station-colored rotating beam with persistence trail
  VIZ_NEBULA,        // two colored blobs slosh on bass; overlap blooms white
  VIZ_FIREFLY,       // speech mode: slew-limited golden presence, moon idle
  VIZ_MODE_COUNT
};

static const char* VIZ_NAMES[VIZ_MODE_COUNT] = {
  "Lantern", "Ocean Breath", "Heartbeat", "Aurora", "Ember Bloom",
  "Prism Slide", "Glint Rain", "Beacon", "Nebula Slosh", "Firefly Talk"
};

class Visualizer {
public:
  void begin() { reset(); }
  void reset() {
    memset(trail_, 0, sizeof(trail_));
    for (int k = 0; k < MAX_BLOOM; k++) bloomAmp_[k] = 0;
    for (int k = 0; k < MAX_GLINT; k++) glintAmp_[k] = 0;
    blobX_[0] = 0.f;  blobX_[1] = VIZ_PI; blobV_[0] = blobV_[1] = 0;
    beaconPhase_ = 0; beaconSpeed_ = 0.08f;
    prismPhase_ = VIZ_PI * 0.5f;
    ffV_ = 0.15f;
    lastBeatCount_ = 0; lastGlintCount_ = 0;
  }
  // stationHue: 0..360 identity color for Beacon (hash the station name)
  void setStationHue(float h) { stationHue_ = h; }

  // t in seconds, dt frame delta; out = float RGB [VIZ_N], linear 0..1 (pre-gamma)
  void render(VizMode mode, float t, float dt, const Features& f, RGBf out[VIZ_N]) {
    for (int i = 0; i < VIZ_N; i++) out[i] = {0, 0, 0};
    switch (mode) {
      case VIZ_LANTERN:  lantern(t, f, out); break;
      case VIZ_OCEAN:    ocean(t, f, out); break;
      case VIZ_HEARTBEAT:heartbeat(t, f, out); break;
      case VIZ_AURORA:   aurora(t, f, out); break;
      case VIZ_EMBER:    ember(t, dt, f, out); break;
      case VIZ_PRISM:    prism(t, dt, f, out); break;
      case VIZ_GLINT:    glintRain(t, dt, f, out); break;
      case VIZ_BEACON:   beacon(t, dt, f, out); break;
      case VIZ_NEBULA:   nebula(t, dt, f, out); break;
      default:           firefly(t, dt, f, out); break;
    }
    for (int i = 0; i < VIZ_N; i++) {
      out[i].r = clampf(out[i].r, 0.f, 1.f);
      out[i].g = clampf(out[i].g, 0.f, 1.f);
      out[i].b = clampf(out[i].b, 0.f, 1.f);
    }
  }

  // final stage for hardware: linear float -> gamma 2.6 -> 8-bit
  static void toBytes(const RGBf in[VIZ_N], uint8_t out[VIZ_N][3], float master = 1.f) {
    for (int i = 0; i < VIZ_N; i++) {
      out[i][0] = g8(in[i].r * master);
      out[i][1] = g8(in[i].g * master);
      out[i][2] = g8(in[i].b * master);
    }
  }

private:
  static inline float ang(int i) { return (float)i * (VIZ_TAU / VIZ_N); }
  static inline uint8_t g8(float x) {
    x = clampf(x, 0.f, 1.f);
    return (uint8_t)(powf(x, 2.6f) * 255.f + 0.5f);
  }
  static uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
  }
  static float hash01(uint32_t x) { return (hash32(x) & 0xFFFFFF) / 16777215.f; }

  // ---- 1 LANTERN --------------------------------------------------------
  void lantern(float t, const Features& f, RGBf out[VIZ_N]) {
    float flick = onoise(t * 1.3f, 7.f);
    float v = 0.28f + 0.14f * flick + 0.22f * f.envS + 0.18f * f.envF;
    float hue = 30.f - 12.f * f.bass + 8.f * f.tilt;
    float c = VIZ_TAU * onoise(t * 0.05f, 3.f);
    for (int i = 0; i < VIZ_N; i++) {
      float vi = v * (0.82f + 0.30f * lobe(ang(i), c, 1.2f));
      out[i] = hsv(hue, 0.85f, clampf(vi, 0.f, 1.f));
    }
  }

  // ---- 2 OCEAN BREATH ---------------------------------------------------
  void ocean(float t, const Features& f, RGBf out[VIZ_N]) {
    float phi = VIZ_TAU * (0.03f * t) + 0.6f * f.envS;
    float v = 0.18f + 0.50f * powf(f.envS, 1.3f) + 0.10f * f.envF;
    float waveC = VIZ_TAU * fractf(0.11f * t) + 5.0f * f.beatAge;  // travels on
    for (int i = 0; i < VIZ_N; i++) {
      float m = 0.5f + 0.5f * sinf(ang(i) - phi);
      float hue = hueLerp(185.f, 158.f, m);
      float vi = v * (0.75f + 0.25f * m);
      vi += 0.38f * f.beat * lobe(ang(i), waveC, 0.6f);
      out[i] = hsv(hue, 0.75f, clampf(vi, 0.f, 1.f));
    }
  }

  // ---- 3 HEARTBEAT ------------------------------------------------------
  static float lubdub(float tau) {
    if (tau < 0) return 0;
    float lub = expf(-tau / 0.10f);
    float d = tau - 0.18f;
    float dub = 0.55f * expf(-(d * d) / (2 * 0.05f * 0.05f));
    return lub + dub;
  }
  void heartbeat(float t, const Features& f, RGBf out[VIZ_N]) {
    float A = lubdub(f.beatAge);
    if (f.beatAge > 1.5f)                       // quiet: calm 55 BPM idle pulse
      A = 0.55f * lubdub(fractf(t / 1.09f) * 1.09f);
    float v = 0.10f + 0.75f * A;
    float hue = hueLerp(350.f, 12.f, clampf(A, 0.f, 1.f));
    float sat = 0.95f - 0.20f * A;
    for (int i = 0; i < VIZ_N; i++) {
      float vi = v * (0.88f + 0.20f * lobe(ang(i), frontAngle_, 1.5f));
      out[i] = hsv(hue, sat, clampf(vi, 0.f, 1.f));
    }
  }

  // ---- 4 AURORA ---------------------------------------------------------
  void aurora(float t, const Features& f, RGBf out[VIZ_N]) {
    const float baseHue[3] = {140.f, 172.f, 275.f};
    float amp[3] = {0.12f + 0.75f * f.mid,
                    0.10f + 0.70f * f.treb,
                    0.12f + 0.80f * clampf(f.mid + f.treb, 0.f, 1.f) * 0.8f};
    float cBass = VIZ_TAU * onoise(t * 0.02f, 11.f);
    for (int i = 0; i < VIZ_N; i++) {
      RGBf acc = {0.02f, 0.02f, 0.06f};                       // deep night floor
      for (int k = 0; k < 3; k++) {
        // curtains drift at perceptibly different speeds (and one retrogrades)
        float spd = 0.045f * (1.f + 0.45f * k) * (k == 1 ? -1.f : 1.f);
        float ck = VIZ_TAU * fractf(spd * t + 0.33f * k);
        float hue = baseHue[k] + 18.f * sinf(0.13f * t + 2.1f * k);
        float w = amp[k] * lobe(ang(i), ck, 0.85f);
        addRGB(acc, hsv(hue, 0.90f, 1.f), w);
      }
      float tiltGain = 1.f + 0.25f * f.bass * cosf(ang(i) - cBass);
      out[i] = {softclip(acc.r * tiltGain), softclip(acc.g * tiltGain),
                softclip(acc.b * tiltGain)};
    }
  }

  // ---- 5 EMBER BLOOM ----------------------------------------------------
  static RGBf firePalette(float x) {                 // 0..1+ -> black/red/orange/gold
    x = clampf(x, 0.f, 1.f);
    if (x < 0.33f) { float u = x / 0.33f; return {0.40f * u, 0.04f * u, 0.f}; }
    if (x < 0.66f) { float u = (x - 0.33f) / 0.33f;
                     return {0.40f + 0.60f * u, 0.04f + 0.38f * u, 0.f}; }
    float u = (x - 0.66f) / 0.34f;
    return {1.f, 0.42f + 0.40f * u, 0.48f * u};
  }
  void ember(float t, float dt, const Features& f, RGBf out[VIZ_N]) {
    if (f.beatCount != lastBeatCount_) {             // spawn a bloom per bass hit
      lastBeatCount_ = f.beatCount;
      int slot = 0; for (int k = 0; k < MAX_BLOOM; k++)
        if (bloomAmp_[k] < bloomAmp_[slot]) slot = k;
      bloomC_[slot] = VIZ_TAU * hash01(f.beatCount * 2654435761u);
      bloomAge_[slot] = 0.f;
      bloomAmp_[slot] = 0.75f + 0.5f * f.bass;
    }
    for (int k = 0; k < MAX_BLOOM; k++)
      if (bloomAmp_[k] > 0.003f) bloomAge_[k] += dt;
    for (int i = 0; i < VIZ_N; i++) {
      float x = 0.10f + 0.10f * f.envS
              + 0.06f * onoise(t * 0.9f + i * 0.7f, (float)i);   // ember shimmer
      for (int k = 0; k < MAX_BLOOM; k++) {
        if (bloomAmp_[k] <= 0.003f) continue;
        float sig = 0.35f + 1.8f * bloomAge_[k];
        float a = bloomAmp_[k] * expf(-bloomAge_[k] / 0.45f);
        x += a * lobe(ang(i), bloomC_[k], sig);
      }
      out[i] = firePalette(x);
    }
  }

  // ---- 6 PRISM SLIDE ----------------------------------------------------
  void prism(float t, float dt, const Features& f, RGBf out[VIZ_N]) {
    // exaggerate the usable tilt range so real spectral motion visibly slides
    float x = clampf((f.tilt - 0.22f) / 0.5f, 0.f, 1.f);
    float target = VIZ_PI * (1.f - x);               // bassy -> 0, bright -> pi
    prismPhase_ += (target - prismPhase_) * (1.f - expf(-dt / 0.6f));
    float hueA = 312.f + 14.f * (onoise(t * 0.07f, 5.f) - 0.5f);
    float hueB = 186.f + 14.f * (onoise(t * 0.07f, 9.f) - 0.5f);
    float v = 0.12f + 0.58f * powf(f.envF, 1.2f) + 0.12f * f.beat;
    for (int i = 0; i < VIZ_N; i++) {
      float m = 0.5f + 0.5f * cosf(ang(i) - prismPhase_);
      float hue = hueLerp(hueA, hueB, m);
      float sat = 0.90f - 0.25f * (1.f - fabsf(2.f * m - 1.f));   // pearly blend zone
      out[i] = hsv(hue, sat, clampf(v, 0.f, 1.f));
    }
  }

  // ---- 7 GLINT RAIN -----------------------------------------------------
  void glintRain(float t, float dt, const Features& f, RGBf out[VIZ_N]) {
    (void)t;
    if (f.glintCount != lastGlintCount_) {
      lastGlintCount_ = f.glintCount;
      int slot = 0; for (int k = 0; k < MAX_GLINT; k++)
        if (glintAmp_[k] < glintAmp_[slot]) slot = k;
      glintC_[slot] = VIZ_TAU * hash01(f.glintCount * 747796405u);
      glintAge_[slot] = 0.f;
      glintAmp_[slot] = 0.65f + 0.5f * f.treb;
      glintSlide_[slot] = (hash01(f.glintCount * 2891336453u) < 0.18f) ? 2.0f : 0.f;
    }
    for (int i = 0; i < VIZ_N; i++) {
      float vi = 0.10f + 0.14f * f.envS;
      out[i] = hsv(250.f, 0.80f, vi);                       // indigo base
    }
    for (int k = 0; k < MAX_GLINT; k++) {
      if (glintAmp_[k] <= 0.003f) continue;
      glintAge_[k] += dt;
      glintC_[k] += glintSlide_[k] * dt;
      float tau = glintSlide_[k] > 0 ? 0.40f : 0.09f;       // drips linger
      float a = glintAmp_[k] * (glintAge_[k] < 0.03f ? 1.f
                : expf(-(glintAge_[k] - 0.03f) / tau));
      if (a < 0.003f) { glintAmp_[k] = 0; continue; }
      RGBf ice = hsv(202.f, 0.25f, 1.f);
      for (int i = 0; i < VIZ_N; i++)
        addRGB(out[i], ice, a * lobe(ang(i), glintC_[k], 0.35f));
    }
    for (int i = 0; i < VIZ_N; i++) {
      out[i].r = softclip(out[i].r); out[i].g = softclip(out[i].g);
      out[i].b = softclip(out[i].b);
    }
  }

  // ---- 8 BEACON ---------------------------------------------------------
  void beacon(float t, float dt, const Features& f, RGBf out[VIZ_N]) {
    (void)t;
    float targetSpeed = 0.08f + 0.50f * clampf(f.onsetRate / 4.f, 0.f, 1.f)
                      * (1.f - f.silence);
    beaconSpeed_ += (targetSpeed - beaconSpeed_) * (1.f - expf(-dt / 1.5f));
    beaconPhase_ = fractf(beaconPhase_ + beaconSpeed_ * dt);
    float psi = beaconPhase_ * VIZ_TAU;
    float amp = 0.25f + 0.60f * f.envF;
    float decay = powf(0.014f, dt);                 // trail: ~86%/frame @60fps
    for (int i = 0; i < VIZ_N; i++) {
      float c = cosf((ang(i) - psi) * 0.5f);
      float b = powf(fmaxf(c, 0.f), 6.f) * amp;     // tight smooth beam
      trail_[i] = fmaxf(trail_[i] * decay, b);
      float x = trail_[i];
      RGBf col = hsv(stationHue_, 0.85f - 0.45f * x, 1.f);  // whiten at core
      RGBf floor = hsv(stationHue_, 0.55f, 0.07f);          // hue-tinted floor
      out[i] = {floor.r + col.r * x, floor.g + col.g * x, floor.b + col.b * x};
      out[i].r = softclip(out[i].r); out[i].g = softclip(out[i].g);
      out[i].b = softclip(out[i].b);
    }
  }

  // ---- 9 NEBULA SLOSH ---------------------------------------------------
  void nebula(float t, float dt, const Features& f, RGBf out[VIZ_N]) {
    for (int k = 0; k < 2; k++) {
      float wander = VIZ_TAU * onoise(0.02f * t, 17.f + 13.f * k);
      float d = fractf((wander - blobX_[k]) / VIZ_TAU + 0.5f) - 0.5f;  // -0.5..0.5
      float springF = 2.2f * d * VIZ_TAU;
      blobV_[k] += springF * dt - blobV_[k] * 1.6f * dt;
      blobX_[k] += blobV_[k] * dt;
    }
    if (f.beatCount != lastBeatCount_) {            // slosh: opposite impulses
      lastBeatCount_ = f.beatCount;
      float imp = 2.4f * (0.5f + 0.5f * f.bass);
      blobV_[0] += imp; blobV_[1] -= imp;
    }
    float amp = 0.45f + 0.30f * f.envS;
    for (int i = 0; i < VIZ_N; i++) {
      RGBf acc = {0.03f, 0.015f, 0.05f};            // dusk floor
      float s0 = 0.55f + 0.15f * onoise(t * 0.2f, 31.f);
      float s1 = 0.55f + 0.15f * onoise(t * 0.2f, 37.f);
      addRGB(acc, hsv(285.f, 0.80f, 1.f), amp * lobe(ang(i), blobX_[0], s0));
      addRGB(acc, hsv(22.f, 0.75f, 1.f),  amp * lobe(ang(i), blobX_[1], s1));
      out[i] = {softclip(acc.r), softclip(acc.g), softclip(acc.b)};
    }
  }

  // ---- 10 FIREFLY TALK --------------------------------------------------
  void firefly(float t, float dt, const Features& f, RGBf out[VIZ_N]) {
    float speech = powf(smoothstepf(f.envF * 1.4f), 1.4f);
    float target = 0.15f + 0.45f * speech;
    // the mode IS its slew limits: rise <= 3/s, fall <= 1.5/s — no strobing
    float maxUp = 3.0f * dt, maxDn = 1.5f * dt;
    ffV_ += clampf(target - ffV_, -maxDn, maxUp);
    float idle = smoothstepf((f.silence - 0.4f) / 0.6f);
    float moonBreath = 0.12f + 0.03f * sinf(VIZ_TAU * 0.05f * t);
    float hue = hueLerp(hueLerp(75.f, 40.f, f.envS), 220.f, idle);
    float sat = lerpf(0.85f, 0.35f, idle);
    float v = lerpf(ffV_, moonBreath, idle);
    for (int i = 0; i < VIZ_N; i++) {
      float micro = 1.f + 0.05f * (onoise(t * 0.8f, 41.f + i * 5.3f) - 0.5f);
      out[i] = hsv(hue, sat, clampf(v * micro, 0.f, 1.f));
    }
  }

  // state
  static const int MAX_BLOOM = 4, MAX_GLINT = 6;
  float trail_[VIZ_N] = {0};
  float bloomC_[MAX_BLOOM] = {0}, bloomAge_[MAX_BLOOM] = {0}, bloomAmp_[MAX_BLOOM] = {0};
  float glintC_[MAX_GLINT] = {0}, glintAge_[MAX_GLINT] = {0}, glintAmp_[MAX_GLINT] = {0},
        glintSlide_[MAX_GLINT] = {0};
  float blobX_[2] = {0, VIZ_PI}, blobV_[2] = {0, 0};
  float beaconPhase_ = 0, beaconSpeed_ = 0.08f, stationHue_ = 210.f;
  float prismPhase_ = VIZ_PI * 0.5f;
  float ffV_ = 0.15f, frontAngle_ = 0.f;
  uint32_t lastBeatCount_ = 0, lastGlintCount_ = 0;
};

} // namespace nh
