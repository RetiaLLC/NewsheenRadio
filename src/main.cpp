// Newsheen Radio — internet radio + LED effects for the Pusheen puck.
//
//   * streams internet radio (MP3 and AAC, http and https) with ICY now-playing
//   * finds stations through radio-browser.info, or takes a pasted stream URL,
//     or takes a hand-off from radio.garden via the browser bookmarklet
//   * runs WLED-style LED effects on the 8-pixel ring, including a VU meter fed
//     from whatever is playing
//   * still plays MP3s off LittleFS, sings the chiptune, and speaks via SAM
//
// Threading: audio owns its own task on core 1 so a slow web request or an LED
// frame can never starve the decoder; LEDs run on core 0; the Arduino loop task
// serves HTTP and reads the button. Nothing blocking lives in loop().
//
// Wiring and the amp's SD/GAIN pins live in board_config.h.
// The 8 NeoPixels need the U5 DIR bodge (see the pusheen-puck skill); the GPIO48
// debug LED also follows the audio so an un-bodged puck still shows signs of life.

#include <Arduino.h>
#include <vector>
#include <algorithm>
#include <LittleFS.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <AudioOutputI2S.h>
#include <AudioGeneratorMP3.h>
#include <AudioGeneratorAAC.h>
#include <AudioFileSourceLittleFS.h>
#include <AudioFileSourceBuffer.h>
#include <AudioFileSourceID3.h>
#include <ESP8266SAM.h>

#include "board_config.h"
#include "chiptune.h"
#include "net_stream.h"
#include "stream_buffer.h"
#include "effects.h"
#include "web_page.h"

#define AP_SSID  "Newsheen-Audio"
#define AP_PASS  "meowmeow"
#define HOSTNAME "newsheen"

// This board brings Wi-Fi up before the rails and USB-CDC enumeration have
// settled, which latches the softAP into "beacons fine, nothing can associate"
// for the whole boot. WLED hits the same race and fixes it with WLED_BOOTUPDELAY;
// 2500 ms is the bench-proven number for this puck (20/20 associations with it).
#define BOOT_SETTLE_MS 2500

// A live stream needs a deep buffer to ride out Wi-Fi hiccups. 192 KB in PSRAM
// is about 12 s at 128 kbps and costs nothing in internal RAM, which the TLS
// handshake and the decoders both need.
#define STREAM_BUF_BYTES (192 * 1024)
// ~3 s at 128 kbps. Enough to ride out jitter without a long silence on tune-in.
#define PREBUFFER_BYTES  (48 * 1024)

#define DIRECTORY_HOST "de1.api.radio-browser.info"   // plain HTTP, no bot-wall

// Default station on a fresh device, and the fallback whenever no station is
// saved. Doubles as the project's hard-path test vector: TLS, a non-standard
// port, HE-AAC (SBR) and ICY metadata all in one URL — if this plays, most of
// the difficult code paths are proven.
#define DEFAULT_STATION_URL  "https://cast2.midiazdx.com.br:7260/stream"
#define DEFAULT_STATION_NAME "Barraco Rap 98.3 FM"

// ---------------------------------------------------------------- audio out
//
// AudioOutputI2S with a tap on the sample stream so the LEDs can follow whatever
// is playing — speech, chiptune, file or stream — without any of them knowing
// about LEDs.
class VUOutputI2S : public AudioOutputI2S {
public:
    // Split the stream into three bands and track their envelopes, so the LEDs
    // can respond to *what* is playing rather than only how loud it is. Done
    // with difference-of-one-pole-lowpass rather than an FFT: it is a handful of
    // multiply-adds per sample, runs inline in the audio path without adding a
    // buffer or a task, and for eight pixels the extra resolution of an FFT
    // would be thrown away anyway.
    bool SetRate(int hz) override {
        rate = hz ? hz : 44100;
        // a = 1 - exp(-2*pi*fc/fs), recomputed because SAM and the chiptune run
        // at 22050 while streams are 44.1/48k; fixed coefficients would put the
        // crossovers in the wrong place for half the sources.
        aBass = 1.0f - expf(-2.0f * PI * 160.0f / rate);
        aMid = 1.0f - expf(-2.0f * PI * 1800.0f / rate);
        return AudioOutputI2S::SetRate(hz);
    }

    bool ConsumeSample(int16_t sample[2]) override {
        bool ok = AudioOutputI2S::ConsumeSample(sample);
        if (!ok) {
            return ok;
        }
        float x = sample[0] * (1.0f / 32768.0f);
        lpB += (x - lpB) * aBass;
        lpM += (x - lpM) * aMid;
        float bass = lpB;
        float mid = lpM - lpB;
        float treb = x - lpM;

        env(envB, fabsf(bass));
        env(envM, fabsf(mid));
        env(envT, fabsf(treb));
        env(envA, fabsf(x));

        // Slow AGC. Stations differ enormously in loudness, and a fixed scale
        // makes a quiet one look broken and a loud one clip flat. Track a
        // decaying peak and normalise to it, with a floor so silence does not
        // divide up into noise.
        if (envA > agc) {
            agc += (envA - agc) * 0.05f;
        } else {
            agc *= 0.999978f;               // ~2 s half-life at 44.1 kHz
        }
        if (agc < 0.02f) {
            agc = 0.02f;
        }

        // Beat: bass envelope well above its own running mean, rate-limited.
        bassAvg += (envB - bassAvg) * 0.00008f;
        if (envB > bassAvg * 1.55f && envB > 0.02f && beatHold == 0) {
            beatFlag = true;
            beatHold = rate / 8;            // 125 ms lockout
        } else if (beatHold) {
            beatHold--;
        }
        return ok;
    }

    // Read-and-clear snapshot for the LED task.
    void snapshot(float &bass, float &mid, float &treble, float &level, bool &beat) {
        float g = 1.0f / agc;
        bass = constrain(envB * g * 1.6f, 0.0f, 1.0f);
        mid = constrain(envM * g * 2.6f, 0.0f, 1.0f);
        treble = constrain(envT * g * 3.4f, 0.0f, 1.0f);
        level = constrain(envA * g, 0.0f, 1.0f);
        beat = beatFlag;
        beatFlag = false;
    }

    int32_t takePeak() {
        int32_t p = (int32_t)(envA * 32767.0f);
        return p;
    }

private:
    void env(volatile float &e, float v) {
        e += (v - e) * (v > e ? 0.25f : 0.0016f);   // fast attack, slow release
    }
    int rate = 44100;
    float aBass = 0.0227f, aMid = 0.2308f;
    float lpB = 0, lpM = 0;
    volatile float envB = 0, envM = 0, envT = 0, envA = 0;
    float agc = 0.05f, bassAvg = 0;
    volatile bool beatFlag = false;
    uint32_t beatHold = 0;
};

bool NetStream::icyEnabled = true;

static VUOutputI2S *out = nullptr;
static AudioGenerator *gen = nullptr;         // whichever generator is live
static AudioGeneratorMP3 *genMp3 = nullptr;
static AudioGeneratorAAC *genAac = nullptr;
static AudioGeneratorChiptune *genTune = nullptr;
static AudioGeneratorTone *genTone = nullptr;

// Volume-indicator tone. The button handler sweeps these; the audio task owns
// the output, so they are the whole interface between the two.
static volatile float toneHz = 440.0f;
static volatile bool toneWanted = false;

static AudioFileSourceLittleFS *srcFile = nullptr;
static AudioFileSourceID3 *srcId3 = nullptr;
static NetStream *srcNet = nullptr;
static AudioFileSourceBuffer *srcBuf = nullptr;   // files only
static StreamBuffer streamRing;                   // live streams
static void *streamBuf = nullptr;                 // PSRAM, allocated once at boot
static uint8_t fileBuf[16 * 1024];                // LittleFS playback (blocking reads are fine there)

enum PlayState { IDLE, SPEAKING, SINGING, PLAYING_FILE, STREAMING };
static volatile PlayState state = IDLE;

// Cross-task status. Fixed buffers rather than String: another task reads these
// every poll, and a String reallocating mid-read is a crash, not a glitch.
static char npTitle[192] = "";                // ICY title, ID3 title or filename
static char npStation[128] = "";              // station or source name
static char curUrl[320] = "";

static Adafruit_NeoPixel strip(NUM_PIXELS, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);
static EffectEngine fx(strip, NUM_PIXELS);
static EffectState fxState;
static WebServer server(80);
static DNSServer dns;
static Preferences prefs;

// Where the device is in its network life. The ring reports this instead of the
// user's effect until we are online, because a first-time user otherwise gets no
// feedback at all — no screen, and "it lit up" is the only signal there is.
enum NetPhase { NET_SETUP, NET_JOINING, NET_ONLINE };
static volatile NetPhase netPhase = NET_SETUP;
static uint32_t onlineAtMs = 0;
static uint32_t netkillUntil = 0;      // bench: deliberate outage window
static volatile float btnVolPreview = -1.0f;   // >=0 while the volume ramp is live
static volatile uint32_t bootHeldMs = 0;       // SW2 hold, for the setup-mode fill
static bool muted = false;

// Single-button gesture thresholds. With no screen, the only way a hold can be
// discoverable is if the ring tells you what releasing now would do.
#define MULTI_PRESS_MS   400     // window to collect further presses
#define LONG_PRESS_MS    600     // SW3 held this long starts the volume ramp
#define BOOT_RESET_MS   5000     // SW2/BOOT held this long enters setup mode
#define VOL_RAMP_PER_S  0.20f    // full 0-100% sweep takes ~5 s, so you can land on a value

// Spoken setup instructions. The password is spelled out because "meow meow"
// heard once from a speech synth is not something you can type with confidence,
// and this device has no screen to check it against.
#define SETUP_SPEECH "Join my wifi network. New sheen audio. " \
                     "The password is meow meow. " \
                     "That is, M, E, O, W, M, E, O, W."

static float volume = 0.5f;
static uint8_t hwGain = 1;                    // 0 = 6 dB, 1 = 9 dB (float), 2 = 12 dB

// Loudness is roughly logarithmic, so a linear gain wastes most of the slider:
// 0.5 sounds almost as loud as 1.0 and everything interesting hides below 0.2.
// A square law tracks perception closely enough and is what the reference radio
// libraries use. Headroom above 1.0 is deliberate — many stations run quiet.
static float gainFor(float v) {
    return constrain(v, 0.0f, 1.0f) * constrain(v, 0.0f, 1.0f) * 1.6f;
}

// ------------------------------------------------------------- command mailbox
enum CmdKind { CMD_NONE, CMD_STOP, CMD_STREAM, CMD_FILE, CMD_SAY, CMD_SING, CMD_NEXT };
struct Command {
    CmdKind kind = CMD_NONE;
    char arg[320] = "";
    char name[128] = "";
};
static Command mailbox;
static SemaphoreHandle_t mailboxLock;

static void post(CmdKind kind, const char *arg = "", const char *name = "") {
    xSemaphoreTake(mailboxLock, portMAX_DELAY);
    mailbox.kind = kind;
    strlcpy(mailbox.arg, arg, sizeof(mailbox.arg));
    strlcpy(mailbox.name, name, sizeof(mailbox.name));
    xSemaphoreGive(mailboxLock);
}

static bool takeCommand(Command &into) {
    if (mailbox.kind == CMD_NONE) {
        return false;
    }
    xSemaphoreTake(mailboxLock, portMAX_DELAY);
    into = mailbox;
    mailbox.kind = CMD_NONE;
    xSemaphoreGive(mailboxLock);
    return into.kind != CMD_NONE;
}

// ------------------------------------------------------------- amp SD / GAIN
static void ampEnable(bool on) {
#if PIN_AMP_SD >= 0
    digitalWrite(PIN_AMP_SD, (on && !muted) ? HIGH : LOW);
#else
    (void)on;
#endif
}

// MAX98357A gain select: a three-state input. Tied high = 6 dB, floating = 9 dB,
// tied low = 12 dB, so a GPIO gets all three.
static void applyHwGain() {
#if PIN_AMP_GAIN >= 0
    switch (hwGain) {
        case 0:
            pinMode(PIN_AMP_GAIN, OUTPUT);
            digitalWrite(PIN_AMP_GAIN, HIGH);
            break;
        case 2:
            pinMode(PIN_AMP_GAIN, OUTPUT);
            digitalWrite(PIN_AMP_GAIN, LOW);
            break;
        default:
            pinMode(PIN_AMP_GAIN, INPUT);
            break;
    }
#endif
}

// ------------------------------------------------------------------- LED task
// Status patterns shown before the user's effect takes over.
static void renderNetStatus(uint32_t now) {
    strip.setBrightness(140);
    if (netPhase == NET_SETUP) {
        // Slow amber breathe: "I am waiting for you."
        float f = 0.15f + 0.85f * (0.5f + 0.5f * sinf(now / 700.0f));
        for (int i = 0; i < NUM_PIXELS; i++) {
            strip.setPixelColor(i, strip.Color(255 * f, 110 * f, 0));
        }
    } else if (netPhase == NET_JOINING) {
        // Blue chase: "I am working on it."
        int head = (now / 90) % NUM_PIXELS;
        for (int i = 0; i < NUM_PIXELS; i++) {
            int d = (head - i + NUM_PIXELS) % NUM_PIXELS;
            float f = powf(0.5f, d);
            strip.setPixelColor(i, strip.Color(0, 80 * f, 255 * f));
        }
    } else {
        // One green sweep on success, then hand over to the user's effect.
        int lit = (now - onlineAtMs) * NUM_PIXELS / 700;
        for (int i = 0; i < NUM_PIXELS; i++) {
            strip.setPixelColor(i, i <= lit ? strip.Color(0, 255, 60) : 0);
        }
    }
    strip.show();
}

static void ledTask(void *) {
    float level = 0.0f;
    for (;;) {
        float bass = 0, mid = 0, treb = 0, lvl = 0;
        bool beat = false;
        if (out) {
            out->snapshot(bass, mid, treb, lvl, beat);
        }
        level = (lvl > level) ? lvl : (level * 0.82f + lvl * 0.18f);
        fxState.audioLevel = level;
        fxState.bass = bass;
        fxState.mid = mid;
        fxState.treble = treb;
        if (beat) {
            fxState.beatAt = millis();
        }

        uint32_t now = millis();
        float volPrev = btnVolPreview;
        uint32_t bootHeld = bootHeldMs;
        if (volPrev >= 0.0f) {
            // Volume ramp: the ring IS the slider. Green low, amber mid, red hot.
            strip.setBrightness(170);
            float lit = volPrev * NUM_PIXELS;
            for (int i = 0; i < NUM_PIXELS; i++) {
                float f = constrain(lit - i, 0.0f, 1.0f);
                uint8_t v = 255 * f;
                if (i >= NUM_PIXELS - 2) {
                    strip.setPixelColor(i, strip.Color(v, 0, 0));
                } else if (i >= NUM_PIXELS - 4) {
                    strip.setPixelColor(i, strip.Color(v, v * 0.55f, 0));
                } else {
                    strip.setPixelColor(i, strip.Color(0, v, v * 0.25f));
                }
            }
            strip.show();
            vTaskDelay(pdMS_TO_TICKS(25));
            continue;
        }
        if (bootHeld > 300) {
            // Filling red toward "forget the network".
            float frac = constrain((float)bootHeld / BOOT_RESET_MS, 0.0f, 1.0f);
            strip.setBrightness(170);
            for (int i = 0; i < NUM_PIXELS; i++) {
                strip.setPixelColor(i, i < (int)(frac * NUM_PIXELS + 0.5f)
                                       ? strip.Color(255, 0, 0) : 0);
            }
            strip.show();
            vTaskDelay(pdMS_TO_TICKS(25));
            continue;
        }
        bool celebrating = (netPhase == NET_ONLINE) && (now - onlineAtMs < 900);
        if (netPhase != NET_ONLINE || celebrating) {
            renderNetStatus(now);
        } else {
            fx.render(fxState, now);
        }
        digitalWrite(PIN_DEBUG_LED, (state == IDLE) ? ((now % 2000) < 60) : (level > 0.08f));
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

// -------------------------------------------------------------- audio helpers
static void teardown() {
    if (gen && gen->isRunning()) {
        gen->stop();
    }
    gen = nullptr;
    // Wrappers do not own what they wrap, so unwind outermost first.
    delete srcBuf;
    srcBuf = nullptr;
    delete srcId3;
    srcId3 = nullptr;
    delete srcFile;
    srcFile = nullptr;
    // Producer first: it holds a pointer to srcNet and must be parked before
    // the socket underneath it is freed.
    streamRing.stop();
    delete srcNet;
    srcNet = nullptr;
    if (out) {
        out->stop();
    }
    ampEnable(false);
    state = IDLE;
    npTitle[0] = npStation[0] = curUrl[0] = 0;
}

static void icyMeta(void *, const char *type, bool, const char *value) {
    Serial.printf("[icy] %s: %s\n", type, value);
    if (!strcmp(type, "StreamTitle")) {
        strlcpy(npTitle, value, sizeof(npTitle));
    }
}

static void id3Meta(void *, const char *type, bool, const char *value) {
    if (!strcmp(type, "Title")) {
        strlcpy(npTitle, value, sizeof(npTitle));
    }
}

static void genStatus(void *, int code, const char *str) {
    Serial.printf("[audio] status %d: %s\n", code, str);
}

// A tenth of the directory answers with a playlist rather than audio. Feeding a
// manifest to the MP3 decoder produces pages of nonsense errors instead of a
// usable message, so classify first.
//   .m3u / .pls  -> a list of stream URLs; take the first and follow it.
//   .m3u8        -> HLS, a different protocol entirely (rolling manifest of
//                   segments, usually AAC in MPEG-TS). Not supported; say so.
enum PlaylistKind { PL_NONE, PL_SIMPLE, PL_HLS };

static PlaylistKind classifyPlaylist(const char *url, const String &contentType) {
    String u = url;
    int q = u.indexOf('?');
    if (q >= 0) {
        u = u.substring(0, q);
    }
    u.toLowerCase();
    if (u.endsWith(".m3u8") || contentType.indexOf("mpegurl") >= 0) {
        // application/vnd.apple.mpegurl and audio/x-mpegurl both land here; the
        // .m3u8 suffix is what separates HLS from a plain list in practice.
        if (u.endsWith(".m3u8")) {
            return PL_HLS;
        }
        return PL_SIMPLE;
    }
    if (u.endsWith(".m3u") || u.endsWith(".pls") ||
        contentType.indexOf("scpls") >= 0 || contentType.indexOf("x-mpegurl") >= 0) {
        return PL_SIMPLE;
    }
    return PL_NONE;
}

// Pull the first playable URL out of an .m3u or .pls body.
static String firstUrlFrom(NetStream *src) {
    char buf[1025];
    uint32_t n = src->read((uint8_t *)buf, sizeof(buf) - 1);
    buf[n] = 0;
    String body(buf);
    int at = 0;
    while (at < (int)body.length()) {
        int nl = body.indexOf('\n', at);
        String line = (nl < 0) ? body.substring(at) : body.substring(at, nl);
        line.trim();
        at = (nl < 0) ? body.length() : nl + 1;
        if (!line.length() || line.startsWith("#")) {
            continue;               // comment or EXTINF metadata
        }
        int eq = line.indexOf('=');  // .pls lines look like File1=http://...
        if (eq >= 0 && line.startsWith("File")) {
            line = line.substring(eq + 1);
            line.trim();
        }
        if (line.startsWith("http://") || line.startsWith("https://")) {
            return line;
        }
    }
    return "";
}

// Open a stream URL and start the right decoder for it.
// Set when a stream fails for a reason that retrying cannot fix — an
// undecodable codec, HLS, an empty playlist. Transient network failures must
// keep retrying; permanent ones must stop immediately and say why, instead of
// sitting on "Reconnecting…" forever for a station that will never work.
static bool lastFailPermanent = false;

static bool startStream(const char *url, const char *name, int depth = 0) {
    lastFailPermanent = false;
    teardown();
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[stream] no internet — set up Wi-Fi first");
        strlcpy(npTitle, "No internet connection", sizeof(npTitle));
        return false;
    }

    srcNet = new NetStream();
    srcNet->RegisterMetadataCB(icyMeta, nullptr);
    srcNet->RegisterStatusCB(genStatus, nullptr);
    Serial.printf("[stream] opening %s\n", url);
    if (!srcNet->open(url)) {
        Serial.println("[stream] open failed");
        strlcpy(npTitle, "Could not open stream", sizeof(npTitle));
        teardown();
        return false;
    }

    // Pick the decoder from the server's Content-Type. Stations mislabel often
    // enough that MP3 is the fallback rather than an error.
    // Resolve a playlist before anything else touches the decoder.
    PlaylistKind pl = classifyPlaylist(url, srcNet->contentType());
    if (pl == PL_HLS) {
        Serial.println("[stream] HLS (.m3u8) is not supported");
        strlcpy(npTitle, "HLS stream — not supported", sizeof(npTitle));
        lastFailPermanent = true;
        teardown();
        return false;
    }
    if (pl == PL_SIMPLE && depth < 2) {
        String inner = firstUrlFrom(srcNet);
        if (!inner.length()) {
            Serial.println("[stream] playlist contained no usable URL");
            strlcpy(npTitle, "Empty playlist", sizeof(npTitle));
            lastFailPermanent = true;
            teardown();
            return false;
        }
        Serial.printf("[stream] playlist -> %s\n", inner.c_str());
        return startStream(inner.c_str(), name, depth + 1);
    }

    // Codec selection. The fallback for an unlabelled stream is MP3, because
    // plenty of servers send application/octet-stream for perfectly ordinary
    // MP3 — but a container we KNOW we cannot decode must be refused here
    // rather than handed to libmad, which does not survive being fed Ogg pages.
    // ESP8266Audio bundles MP3, AAC, FLAC, Opus and WAV decoders but no Vorbis,
    // and "OGG stream" in the wild almost always means Ogg Vorbis.
    String ct = srcNet->contentType();
    const char *unsupported = nullptr;
    if (ct.indexOf("vorbis") >= 0) {
        unsupported = "OGG Vorbis";
    } else if (ct.indexOf("ogg") >= 0) {
        unsupported = "OGG";
    } else if (ct.indexOf("flac") >= 0) {
        unsupported = "FLAC";
    } else if (ct.indexOf("wav") >= 0 || ct.indexOf("x-wav") >= 0) {
        unsupported = "WAV";
    } else if (ct.indexOf("opus") >= 0) {
        unsupported = "Opus";
    }
    if (unsupported) {
        Serial.printf("[stream] %s is not supported (content-type '%s')\n",
                      unsupported, ct.c_str());
        snprintf(npTitle, sizeof(npTitle), "%s — not supported", unsupported);
        lastFailPermanent = true;
        char keep[192];
        strlcpy(keep, npTitle, sizeof(keep));
        teardown();
        strlcpy(npTitle, keep, sizeof(npTitle));   // teardown clears it; the user needs the reason
        return false;
    }

    bool isAac = ct.indexOf("aac") >= 0 || ct.indexOf("mp4") >= 0 || ct.indexOf("3gpp") >= 0;
    Serial.printf("[stream] content-type '%s' -> %s\n", ct.c_str(), isAac ? "AAC" : "MP3");

    if (!streamRing.start(srcNet)) {
        Serial.println("[stream] could not start the prefetch task");
        teardown();
        return false;
    }
    // Start with a cushion so the first seconds aren't fighting the network.
    uint32_t t0 = millis();
    bool ready = streamRing.prebuffer(PREBUFFER_BYTES, 8000);
    Serial.printf("[stream] prebuffered %u bytes in %u ms%s\n", streamRing.available(),
                  millis() - t0, ready ? "" : " (timed out, starting anyway)");

    gen = isAac ? (AudioGenerator *)genAac : (AudioGenerator *)genMp3;
    gen->RegisterStatusCB(genStatus, nullptr);
    out->SetGain(gainFor(volume));
    if (!gen->begin(&streamRing, out)) {
        Serial.println("[stream] decoder refused the stream");
        strlcpy(npTitle, "Stream format not supported", sizeof(npTitle));
        teardown();
        return false;
    }

    ampEnable(true);
    state = STREAMING;
    strlcpy(curUrl, url, sizeof(curUrl));
    strlcpy(npStation, (name && *name) ? name : srcNet->stationName().c_str(), sizeof(npStation));
    if (!npStation[0]) {
        strlcpy(npStation, "Internet radio", sizeof(npStation));
    }
    strlcpy(npTitle, npStation, sizeof(npTitle));   // until ICY says otherwise
    prefs.putString("lastUrl", url);
    prefs.putString("lastName", npStation);
    return true;
}

static bool startFile(const char *path) {
    teardown();
    if (!LittleFS.exists(path)) {
        Serial.printf("[file] no such file: %s\n", path);
        return false;
    }
    srcFile = new AudioFileSourceLittleFS(path);
    srcId3 = new AudioFileSourceID3(srcFile);
    srcId3->RegisterMetadataCB(id3Meta, nullptr);
    srcBuf = new AudioFileSourceBuffer(srcId3, fileBuf, sizeof(fileBuf));
    gen = genMp3;
    gen->RegisterStatusCB(genStatus, nullptr);
    out->SetGain(gainFor(volume));
    if (!gen->begin(srcBuf, out)) {
        Serial.printf("[file] decoder refused %s\n", path);
        teardown();
        return false;
    }
    ampEnable(true);
    state = PLAYING_FILE;
    strlcpy(npStation, "Local file", sizeof(npStation));
    strlcpy(npTitle, path, sizeof(npTitle));
    Serial.printf("[file] playing %s\n", path);
    return true;
}

static void startSing() {
    teardown();
    out->SetGain(gainFor(volume));
    if (!genTune->begin(nullptr, out)) {
        return;
    }
    gen = genTune;
    ampEnable(true);
    state = SINGING;
    strlcpy(npStation, "Chiptune", sizeof(npStation));
    strlcpy(npTitle, "Newsheen's Little Song", sizeof(npTitle));
}

// SAM is blocking; it only ever runs inside the audio task.
static void speak(const char *text) {
    teardown();
    state = SPEAKING;
    strlcpy(npStation, "Speech", sizeof(npStation));
    strlcpy(npTitle, text, sizeof(npTitle));
    Serial.printf("[say] %s\n", text);

    ESP8266SAM sam;
    sam.SetVoice(ESP8266SAM::VOICE_SAM);
    sam.SetPitch(58);
    sam.SetSpeed(80);
    sam.SetMouth(140);
    sam.SetThroat(120);

    out->SetGain(gainFor(volume));
    ampEnable(true);
    delay(5);
    sam.Say(out, text);
    out->flush();
    ampEnable(false);
    out->stop();
    state = IDLE;
    npTitle[0] = npStation[0] = 0;
}

// ------------------------------------------------------------------ favourites
static JsonDocument favDoc;

static void saveFavs();

static void loadFavs() {
    favDoc.to<JsonArray>();
    File f = LittleFS.open("/favs.json", "r");
    if (f) {
        if (deserializeJson(favDoc, f)) {
            favDoc.to<JsonArray>();     // corrupt file: start clean rather than fail
        }
        f.close();
    }
    if (!favDoc.is<JsonArray>()) {
        favDoc.to<JsonArray>();
    }
    if (favDoc.as<JsonArray>().size() == 0) {
        JsonObject o = favDoc.as<JsonArray>().add<JsonObject>();
        o["name"] = DEFAULT_STATION_NAME;
        o["url"] = DEFAULT_STATION_URL;
        o["codec"] = "AAC+";
        o["bitrate"] = 128;
        saveFavs();
    }
}

static void saveFavs() {
    File f = LittleFS.open("/favs.json", "w");
    if (f) {
        serializeJson(favDoc, f);
        f.close();
    }
}

// -------------------------------------------------------------- audio task
//
// Reconnect is a state machine rather than a loop inside the "generator died"
// branch. The naive version only retried while the decoder was still alive, so
// the first failed attempt tore everything down to IDLE and playback never came
// back — any outage longer than one retry interval was permanent. Now a pending
// retry survives teardown and is driven from the task's main loop.
static char retryUrl[320] = "";
static char retryName[128] = "";
static int retriesLeft = 0;
static uint32_t retryAtMs = 0;

static void scheduleRetry() {
    if (retriesLeft <= 0) {
        return;
    }
    int attempt = 21 - retriesLeft;
    uint32_t wait = min(1000 * attempt, 15000);     // 1s, 2s, 3s … capped at 15s
    retryAtMs = millis() + wait;
    Serial.printf("[stream] retry in %u ms (%d left)\n", wait, retriesLeft);
    strlcpy(npTitle, "Reconnecting…", sizeof(npTitle));
}

static void audioTask(void *) {
    for (;;) {
        Command c;
        if (takeCommand(c)) {
            switch (c.kind) {
                case CMD_STOP:
                    retriesLeft = 0;
                    teardown();
                    break;
                case CMD_STREAM:
                    strlcpy(retryUrl, c.arg, sizeof(retryUrl));
                    strlcpy(retryName, c.name, sizeof(retryName));
                    if (startStream(c.arg, c.name)) {
                        retriesLeft = 20;
                        retryAtMs = 0;
                    } else if (lastFailPermanent) {
                        retriesLeft = 0;            // will never work; say so and stop
                    } else {
                        retriesLeft = 20;           // may just be briefly unreachable
                        scheduleRetry();
                    }
                    break;
                case CMD_FILE:
                    retriesLeft = 0;
                    startFile(c.arg);
                    break;
                case CMD_SAY:
                    retriesLeft = 0;
                    speak(c.arg);
                    break;
                case CMD_SING:
                    retriesLeft = 0;
                    startSing();
                    break;
                default:
                    break;
            }
        }

        // Volume-indicator tone, only when nothing else is playing: while a
        // station is on, the music itself is the reference and a tone on top of
        // it would just be noise.
        if (toneWanted && state == IDLE && gen != genTone) {
            out->SetGain(gainFor(volume));
            if (genTone->begin(nullptr, out)) {
                gen = genTone;
                ampEnable(true);
            }
        } else if (!toneWanted && gen == genTone) {
            genTone->stop();
            gen = nullptr;
            ampEnable(false);
            out->stop();
        }

        if (gen && gen->isRunning()) {
            // Yield every pass. Once the I2S DMA is full, gen->loop() returns
            // immediately without blocking, so this task — priority 2 on core 1 —
            // would spin and starve the priority-1 Arduino loop task pinned to the
            // same core. That silently kills the web UI, the serial CLI and the
            // button for as long as something is playing.
            vTaskDelay(1);
            if (!gen->loop()) {
                bool wasStream = (state == STREAMING);
                Serial.println(wasStream ? "[stream] source ended" : "[audio] finished");
                teardown();
                if (wasStream && retriesLeft > 0) {
                    scheduleRetry();
                }
            }
        } else if (retriesLeft > 0 && retryAtMs && millis() >= retryAtMs) {
            // Only attempt once the link is actually back — otherwise every
            // outage burns the whole retry budget in the first few seconds.
            if (WiFi.status() != WL_CONNECTED) {
                retryAtMs = millis() + 1000;
            } else if (startStream(retryUrl, retryName)) {
                retriesLeft = 20;
                retryAtMs = 0;
                Serial.println("[stream] reconnected");
            } else if (lastFailPermanent) {
                retriesLeft = 0;
            } else {
                retriesLeft--;
                scheduleRetry();
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

// ---------------------------------------------------------------- directory
static String urlEncode(const String &s) {
    static const char *hex = "0123456789ABCDEF";
    String o;
    o.reserve(s.length() * 3);
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
            o += c;
        } else {
            o += '%';
            o += hex[(c >> 4) & 0xF];
            o += hex[c & 0xF];
        }
    }
    return o;
}

// radio-browser.info answers over plain HTTP, so no TLS is needed just to
// search — that matters because a TLS session and a decoder competing for
// internal RAM is exactly what makes streaming devices flaky.
//
// One query against one field, appending de-duplicated rows to `arr`.
static void queryField(const char *field, const String &query, JsonArray arr, int limit) {
    HTTPClient http;
    String url = String("http://" DIRECTORY_HOST "/json/stations/search?hidebroken=true"
                        "&order=clickcount&reverse=true&limit=") + limit +
                 "&" + field + "=" + urlEncode(query);
    http.setUserAgent("Newsheen/1.0");
    http.setConnectTimeout(6000);
    http.setTimeout(9000);
    if (!http.begin(url)) {
        return;
    }
    if (http.GET() != 200) {
        http.end();
        return;
    }

    // Keep only the fields the UI shows — raw rows carry ~40 each, and twenty of
    // those will not fit in RAM.
    JsonDocument filter;
    for (const char *k : {"name", "url_resolved", "codec", "bitrate", "country",
                          "geo_lat", "geo_long"}) {
        filter[0][k] = true;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream(),
                                               DeserializationOption::Filter(filter));
    http.end();
    if (err) {
        Serial.printf("[dir] parse failed (%s): %s\n", field, err.c_str());
        return;
    }

    for (JsonObject s : doc.as<JsonArray>()) {
        const char *u = s["url_resolved"];
        if (!u || !*u) {
            continue;
        }
        bool dup = false;
        for (JsonObject e : arr) {
            if (!strcmp(u, e["url"] | "")) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        JsonObject o = arr.add<JsonObject>();
        o["name"] = s["name"];
        o["url"] = u;
        o["codec"] = s["codec"];
        o["bitrate"] = s["bitrate"];
        o["country"] = s["country"];
        if (!s["geo_lat"].isNull() && !s["geo_long"].isNull()) {
            o["lat"] = s["geo_lat"];
            o["lon"] = s["geo_long"];
        }
    }
}

// The box invites "name, genre or country", so honour all three. radio-browser
// ANDs its parameters, so covering them means separate queries — done in
// decreasing likelihood and stopped early once there is enough to show.
static String searchStations(const String &query) {
    JsonDocument slim;
    JsonArray arr = slim.to<JsonArray>();

    queryField("name", query, arr, 20);
    if (arr.size() < 8) {
        queryField("tag", query, arr, 12);
    }
    if (arr.size() < 8) {
        queryField("country", query, arr, 12);
    }
    Serial.printf("[dir] '%s' -> %u stations\n", query.c_str(), (unsigned)arr.size());

    String outStr;
    serializeJson(slim, outStr);
    return outStr;
}

// Pick a station at random from the directory, filtered to what this device can
// actually sustain: MP3/AAC only, and at or below the measured ~190 kbps
// throughput ceiling. A "random" button that lands on a 320 kbps HLS stream
// half the time is not a feature.
static void applyMute();
static void nextFavourite();

static bool tuneRandomStation() {
    HTTPClient http;
    // NOT order=random: that endpoint is cached and hands back the same 25 rows
    // call after call, so the button would circle one small pool forever. A
    // random offset into the popularity ordering varies the pool for real.
    uint32_t off = esp_random() % 900;
    String url = "http://" DIRECTORY_HOST "/json/stations/search?hidebroken=true"
                 "&order=clickcount&reverse=true&limit=25&offset=" + String(off);
    http.setUserAgent("Newsheen/1.0");
    http.setConnectTimeout(6000);
    http.setTimeout(9000);
    if (!http.begin(url)) {
        return false;
    }
    if (http.GET() != 200) {
        http.end();
        return false;
    }
    JsonDocument filter;
    for (const char *k : {"name", "url_resolved", "codec", "bitrate"}) {
        filter[0][k] = true;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream(),
                                               DeserializationOption::Filter(filter));
    http.end();
    if (err) {
        return false;
    }

    std::vector<JsonObject> ok;
    for (JsonObject st : doc.as<JsonArray>()) {
        const char *u = st["url_resolved"];
        String codec = st["codec"] | "";
        int br = st["bitrate"] | 0;
        if (!u || !*u) {
            continue;
        }
        String lower = u;
        lower.toLowerCase();
        if (lower.indexOf(".m3u8") >= 0) {
            continue;                       // HLS: unsupported
        }
        codec.toUpperCase();
        if (codec.indexOf("MP3") < 0 && codec.indexOf("AAC") < 0) {
            continue;                       // no Vorbis/FLAC decoder path
        }
        if (br > 192) {
            continue;                       // beyond what we can stream cleanly
        }
        ok.push_back(st);
    }
    if (ok.empty()) {
        return false;
    }
    JsonObject pick = ok[esp_random() % ok.size()];
    Serial.printf("[btn] random -> %s (%s %dk)\n", (const char *)(pick["name"] | "?"),
                  (const char *)(pick["codec"] | "?"), (int)(pick["bitrate"] | 0));
    post(CMD_STREAM, pick["url_resolved"] | "", pick["name"] | "");
    return true;
}

// ------------------------------------------------------------------ Wi-Fi
static void connectSta(const String &ssid, const String &pass) {
    if (!ssid.length()) {
        netPhase = NET_SETUP;
        return;
    }
    Serial.printf("[wifi] joining %s\n", ssid.c_str());
    netPhase = NET_JOINING;
    WiFi.begin(ssid.c_str(), pass.c_str());
    uint32_t deadline = millis() + 15000;
    while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
        delay(200);
    }
    if (WiFi.status() == WL_CONNECTED) {
        // Modem sleep parks the radio between DTIM beacons. It saves power on a
        // sensor that wakes to send a reading; on a continuous stream it adds
        // latency to every packet and caps throughput well below what the link
        // can carry. This device is mains-powered and streaming is the whole
        // point, so keep the radio awake.
        WiFi.setSleep(false);
        Serial.printf("[wifi] connected, IP %s (%d dBm), AP moved to channel %d\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI(), WiFi.channel());
        netPhase = NET_ONLINE;
        onlineAtMs = millis();
    } else {
        // Nothing to recover from: the AP never went away, so the user can just
        // rejoin it and try different credentials.
        Serial.println("[wifi] join failed — AP stays up for reconfiguration");
        netPhase = NET_SETUP;
        static bool toldThem = false;
        if (!toldThem) {
            toldThem = true;
            post(CMD_SAY, "I could not join that network. " SETUP_SPEECH
                          " Or hold my button to start over.");
        }
    }
}

// -------------------------------------------------------------------- web
static void sendJson(const String &j) {
    server.sendHeader("Access-Control-Allow-Origin", "*");   // for the bookmarklet
    server.send(200, "application/json", j);
}

static void handleStatus() {
    JsonDocument d;
    d["state"] = (int)state;
    d["title"] = npTitle;
    d["station"] = npStation;
    d["vol"] = volume;
    d["heap"] = ESP.getFreeHeap();
    JsonObject w = d["wifi"].to<JsonObject>();
    bool sta = WiFi.status() == WL_CONNECTED;
    w["sta"] = sta;
    w["ssid"] = sta ? WiFi.SSID() : "";
    w["ip"] = sta ? WiFi.localIP().toString() : String("");
    w["rssi"] = sta ? WiFi.RSSI() : 0;
    w["apip"] = WiFi.softAPIP().toString();
    String s;
    serializeJson(d, s);
    sendJson(s);
}

static void handleFx() {
    if (server.hasArg("effect")) {
        fxState.effect = constrain(server.arg("effect").toInt(), 0, FX_COUNT - 1);
    }
    if (server.hasArg("bri")) {
        fxState.brightness = constrain(server.arg("bri").toInt(), 1, 255);
    }
    if (server.hasArg("speed")) {
        fxState.speed = constrain(server.arg("speed").toInt(), 0, 255);
    }
    if (server.hasArg("r")) {
        fxState.r = constrain(server.arg("r").toInt(), 0, 255);
        fxState.g = constrain(server.arg("g").toInt(), 0, 255);
        fxState.b = constrain(server.arg("b").toInt(), 0, 255);
    }
    if (server.args()) {
        prefs.putBytes("fx", (const void *)&fxState, sizeof(fxState));
    }

    JsonDocument d;
    d["effect"] = fxState.effect;
    d["bri"] = fxState.brightness;
    d["speed"] = fxState.speed;
    d["r"] = fxState.r;
    d["g"] = fxState.g;
    d["b"] = fxState.b;
    JsonArray names = d["names"].to<JsonArray>();
    for (int i = 0; i < FX_COUNT; i++) {
        names.add(EFFECT_NAMES[i]);
    }
    String s;
    serializeJson(d, s);
    sendJson(s);
}

static std::vector<String> listMp3s() {
    std::vector<String> files;
    File root = LittleFS.open("/");
    for (File f = root.openNextFile(); f; f = root.openNextFile()) {
        String n = f.name();
        if (!n.startsWith("/")) {
            n = "/" + n;
        }
        String lower = n;
        lower.toLowerCase();
        if (!f.isDirectory() && lower.endsWith(".mp3")) {
            files.push_back(n);
        }
    }
    std::sort(files.begin(), files.end());      // LittleFS order is not stable
    return files;
}

static File uploadFile;
static void handleUploadData() {
    HTTPUpload &up = server.upload();
    if (up.status == UPLOAD_FILE_START) {
        post(CMD_STOP);                          // decoding while writing flash underruns
        String name = up.filename;
        if (!name.startsWith("/")) {
            name = "/" + name;
        }
        Serial.printf("[upload] %s\n", name.c_str());
        uploadFile = LittleFS.open(name, "w");
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (uploadFile) {
            uploadFile.write(up.buf, up.currentSize);
        }
    } else if (up.status == UPLOAD_FILE_END) {
        if (uploadFile) {
            Serial.printf("[upload] done, %u bytes\n", (unsigned)up.totalSize);
            uploadFile.close();
        }
    }
}

static void setupWeb() {
    server.on("/", []() {
        server.send_P(200, "text/html", INDEX_HTML);
    });
    server.on("/api/status", handleStatus);
    server.on("/api/fx", handleFx);

    // ---- on-device catalogue (radio.garden, ~38k stations on LittleFS) --------
    // Streamed, never assembled in RAM: a country's worth of stations is tens of
    // KB of JSON and the heap has less than 100 KB free with a TLS session up.
    server.on("/api/countries", []() {
        File f = LittleFS.open("/countries.idx", "r");
        if (!f) {
            sendJson("[]");
            return;
        }
        server.sendHeader("Access-Control-Allow-Origin", "*");
        server.setContentLength(CONTENT_LENGTH_UNKNOWN);
        server.send(200, "application/json", "");
        server.sendContent("[");
        bool first = true;
        while (f.available()) {
            String line = f.readStringUntil('\n');
            int t1 = line.indexOf('\t');
            int t2 = line.indexOf('\t', t1 + 1);
            if (t1 < 0 || t2 < 0) {
                continue;
            }
            String name = line.substring(0, t1);
            name.replace("\"", "'");
            server.sendContent(String(first ? "" : ",") + "{\"name\":\"" + name +
                               "\",\"off\":" + line.substring(t1 + 1, t2) +
                               ",\"n\":" + line.substring(t2 + 1) + "}");
            first = false;
        }
        server.sendContent("]");
        server.sendContent("");
        f.close();
    });

    server.on("/api/catalogue", []() {
        uint32_t off = server.arg("off").toInt();
        int want = server.arg("n").toInt();
        if (want <= 0 || want > 400) {
            want = 200;
        }
        File f = LittleFS.open("/stations.tsv", "r");
        if (!f) {
            sendJson("[]");
            return;
        }
        // sample=N walks evenly spaced offsets across the whole file. Because the
        // catalogue is sorted by country, even spacing gives a worldwide scatter —
        // which is what the globe should open with instead of nothing.
        int sample = server.arg("sample").toInt();
        uint32_t fsize = f.size();
        uint32_t stride = (sample > 0 && fsize) ? fsize / (uint32_t)sample : 0;
        if (sample > 0) {
            want = sample;
        }
        f.seek(off);
        server.sendHeader("Access-Control-Allow-Origin", "*");
        server.setContentLength(CONTENT_LENGTH_UNKNOWN);
        server.send(200, "application/json", "");

        // Block reads, not readStringUntil(). That reads a byte at a time through
        // the whole LittleFS stack and cost 7.6 s for 60 rows; pulling 2 KB at a
        // time and splitting in memory does the same work in a fraction of it.
        // Output is likewise accumulated and flushed in chunks rather than one
        // sendContent() per station.
        static char buf[2048];
        String line, out;
        out.reserve(2600);
        int sent = 0;
        bool first = true;
        bool done = false;
        bool overlong = false;

        uint32_t nextSeek = stride;
        while (!done && f.available()) {
            if (stride) {
                // one row per stride, then jump; skip the partial line we land on
                f.seek(nextSeek);
                nextSeek += stride;
                if (nextSeek >= fsize) {
                    done = true;
                }
                line = "";
                int c;
                while ((c = f.read()) >= 0 && c != '\n') {
                }
            }
            int n = f.readBytes(buf, sizeof(buf));
            if (n <= 0) {
                break;
            }
            for (int i = 0; i < n && !done; i++) {
                if (buf[i] != '\n') {
                    // Cap well above the longest real row (429). Truncating a row
                    // silently emits half a JSON object and breaks the whole
                    // response, so an over-long line is marked and skipped whole.
                    if (line.length() < 700) {
                        line += buf[i];
                    } else {
                        overlong = true;
                    }
                    continue;
                }
                // name \t place \t country \t lat \t lon \t url
                int p[5], at = 0;
                bool ok = !overlong;
                overlong = false;
                for (int k = 0; ok && k < 5; k++) {
                    at = line.indexOf('\t', at);
                    if (at < 0) {
                        ok = false;
                        break;
                    }
                    p[k] = at++;
                }
                if (ok) {
                    String name = line.substring(0, p[0]);
                    String place = line.substring(p[0] + 1, p[1]);
                    name.replace("\"", "'");
                    place.replace("\"", "'");
                    out += first ? "[" : ",";
                    first = false;
                    out += "{\"name\":\"";
                    out += name;
                    out += "\",\"place\":\"";
                    out += place;
                    out += "\",\"country\":\"";
                    out += line.substring(p[1] + 1, p[2]);
                    out += "\",\"lat\":";
                    out += (p[3] - p[2] > 1) ? line.substring(p[2] + 1, p[3]) : String("null");
                    out += ",\"lon\":";
                    out += (p[4] - p[3] > 1) ? line.substring(p[3] + 1, p[4]) : String("null");
                    out += ",\"url\":\"";
                    out += line.substring(p[4] + 1);
                    out += "\"}";
                    if (++sent >= want) {
                        done = true;
                    }
                    if (out.length() > 2000) {
                        server.sendContent(out);
                        out = "";
                    }
                    if (stride) {
                        line = "";
                        break;              // one row per stride
                    }
                }
                line = "";
            }
        }
        if (first) {
            out += "[";
        }
        out += "]";
        server.sendContent(out);
        server.sendContent("");
        f.close();
    });

    server.on("/api/search", []() {
        sendJson(searchStations(server.arg("q")));
    });

    server.on("/api/tune", []() {
        post(CMD_STREAM, server.arg("u").c_str(), server.arg("n").c_str());
        sendJson("{\"ok\":true}");
    });

    // Top-level navigation target for the radio.garden bookmarklet. It has to be
    // a real page rather than a fetch: radio.garden is https and the puck is
    // http, so a cross-origin fetch would be blocked as mixed content, while a
    // plain navigation is not.
    server.on("/tune", []() {
        String u = server.arg("u"), n = server.arg("n");
        post(CMD_STREAM, u.c_str(), n.c_str());
        String h = "<!doctype html><meta charset=utf-8>"
                   "<meta name=viewport content='width=device-width,initial-scale=1'>"
                   "<body style='background:#141018;color:#f0e6f5;font:16px system-ui;padding:40px;text-align:center'>"
                   "<h2>Tuning the Sheen…</h2><p style='color:#a595b0'>";
        h += n.length() ? n : u;
        h += "</p><p><a style='color:#c9a7e0' href='/'>Open the full control panel</a></p>";
        server.send(200, "text/html", h);
    });

    server.on("/api/stop", []() {
        post(CMD_STOP);
        sendJson("{\"ok\":true}");
    });
    server.on("/api/sing", []() {
        post(CMD_SING);
        sendJson("{\"ok\":true}");
    });
    server.on("/api/say", []() {
        String t = server.arg("t");
        post(CMD_SAY, t.length() ? t.c_str() : "Hello!");
        sendJson("{\"ok\":true}");
    });
    server.on("/api/play", []() {
        post(CMD_FILE, server.arg("f").c_str());
        sendJson("{\"ok\":true}");
    });
    server.on("/api/vol", []() {
        volume = constrain(server.arg("v").toFloat() / 100.0f, 0.0f, 1.0f);
        if (out) {
            out->SetGain(gainFor(volume));
        }
        prefs.putFloat("vol", volume);
        sendJson("{\"ok\":true}");
    });

    server.on("/api/files", []() {
        JsonDocument d;
        JsonArray a = d.to<JsonArray>();
        for (const String &f : listMp3s()) {
            File fh = LittleFS.open(f);
            JsonObject o = a.add<JsonObject>();
            o["name"] = f;
            o["size"] = fh ? fh.size() : 0;
        }
        String s;
        serializeJson(d, s);
        sendJson(s);
    });
    server.on("/api/rm", []() {
        post(CMD_STOP);
        LittleFS.remove(server.arg("f"));
        sendJson("{\"ok\":true}");
    });
    server.on("/upload", HTTP_POST, []() {
        server.sendHeader("Location", "/");
        server.send(303);
    }, handleUploadData);

    server.on("/api/fav", []() {
        String s;
        serializeJson(favDoc, s);
        sendJson(s);
    });
    server.on("/api/fav/add", []() {
        String u = server.arg("u");
        for (JsonObject o : favDoc.as<JsonArray>()) {
            if (u == (const char *)o["url"]) {
                sendJson("{\"ok\":true}");
                return;                          // already saved
            }
        }
        JsonObject o = favDoc.as<JsonArray>().add<JsonObject>();
        o["name"] = server.arg("n");
        o["url"] = u;
        o["codec"] = server.arg("c");
        o["bitrate"] = server.arg("b").toInt();
        saveFavs();
        sendJson("{\"ok\":true}");
    });
    server.on("/api/fav/del", []() {
        String u = server.arg("u");
        JsonArray a = favDoc.as<JsonArray>();
        for (size_t i = 0; i < a.size(); i++) {
            if (u == (const char *)a[i]["url"]) {
                a.remove(i);
                break;
            }
        }
        saveFavs();
        sendJson("{\"ok\":true}");
    });

    server.on("/api/scan", []() {
        int n = WiFi.scanNetworks();
        JsonDocument d;
        JsonArray a = d.to<JsonArray>();
        for (int i = 0; i < n && i < 20; i++) {
            JsonObject o = a.add<JsonObject>();
            o["ssid"] = WiFi.SSID(i);
            o["rssi"] = WiFi.RSSI(i);
            o["lock"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        }
        WiFi.scanDelete();
        String s;
        serializeJson(d, s);
        sendJson(s);
    });
    server.on("/api/wifi", []() {
        String ssid = server.arg("ssid"), pass = server.arg("pass");
        prefs.putString("ssid", ssid);
        prefs.putString("pass", pass);
        sendJson("{\"ok\":true}");
        connectSta(ssid, pass);
    });

    // Captive-portal catch-all. Paired with the wildcard DNS server, this is what
    // makes iOS and Android pop their "sign in to network" sheet on join: their
    // connectivity probe expects a specific body from a known URL, and anything
    // else — a redirect here — is read as "there is a portal to show".
    server.onNotFound([]() {
        if (netPhase == NET_ONLINE) {
            server.send(404, "text/plain", "not found");
            return;
        }
        server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/");
        server.send(302, "text/plain", "");
    });
    server.begin();
}

// ------------------------------------------------------------------- setup
void setup() {
    // Mute the amp before anything else — SD low holds the output stage down so
    // the rail coming up doesn't thump through the speaker.
#if PIN_AMP_SD >= 0
    pinMode(PIN_AMP_SD, OUTPUT);
    digitalWrite(PIN_AMP_SD, LOW);
#endif
    applyHwGain();

    Serial.begin(115200);
    pinMode(PIN_DEBUG_LED, OUTPUT);
    pinMode(PIN_BUTTON, INPUT_PULLUP);
    pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);

    strip.begin();
    strip.clear();
    strip.show();

    LittleFS.begin(true);
    loadFavs();

    prefs.begin("newsheen", false);
    volume = prefs.getFloat("vol", 0.5f);
    size_t got = prefs.getBytes("fx", &fxState, sizeof(fxState));
    if (got != sizeof(fxState)) {
        fxState = EffectState();          // first boot, or the struct changed shape
    }

    streamBuf = ps_malloc(STREAM_BUF_BYTES + 1);
    if (!streamBuf) {
        // No PSRAM (or it is octal and the pins are gone): fall back to a small
        // internal buffer. Streaming will be far more hiccup-prone.
        streamBuf = malloc(32 * 1024 + 1);
        Serial.println("[boot] WARNING: PSRAM allocation failed, using 32 KB internal");
    }
    if (!streamRing.init((uint8_t *)streamBuf,
                         ESP.getPsramSize() ? STREAM_BUF_BYTES : 32 * 1024)) {
        Serial.println("[boot] FATAL: stream ring would not initialise");
    }

    mailboxLock = xSemaphoreCreateMutex();

    out = new VUOutputI2S();
    out->SetPinout(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DIN);
    out->SetGain(gainFor(volume));
    genMp3 = new AudioGeneratorMP3();
    genAac = new AudioGeneratorAAC();
    genTune = new AudioGeneratorChiptune();
    genTone = new AudioGeneratorTone(&toneHz);

    xTaskCreatePinnedToCore(ledTask, "leds", 4096, nullptr, 1, nullptr, 0);
    // 32 KB, not 16: the helix AAC decoder uses substantially more stack than
    // libmad, and overflowing it here corrupts the return address rather than
    // tripping the canary cleanly (the panic shows "BREAK instr" and a
    // |<-CORRUPTED backtrace, which is what an overflow looks like when it
    // lands mid-frame).
    xTaskCreatePinnedToCore(audioTask, "audio", 32768, nullptr, 2, nullptr, 1);

    delay(BOOT_SETTLE_MS);                // see the note at the top — do not remove
    WiFi.mode(WIFI_AP_STA);               // AP always up, so you can never lock yourself out
    WiFi.setHostname(HOSTNAME);
    WiFi.setAutoReconnect(true);
    WiFi.softAP(AP_SSID, AP_PASS);
    // Wildcard DNS: every lookup resolves to us, which is half of what makes a
    // phone show its captive-portal sheet (the other half is the 302 in
    // server.onNotFound). Without it, a first-time user has to somehow know to
    // type an IP address into a browser.
    dns.setErrorReplyCode(DNSReplyCode::NoError);
    dns.start(53, "*", WiFi.softAPIP());
    connectSta(prefs.getString("ssid", ""), prefs.getString("pass", ""));
    if (MDNS.begin(HOSTNAME)) {
        MDNS.addService("http", "tcp", 80);
    }
    setupWeb();

    Serial.println();
    Serial.println("Newsheen Radio");
    Serial.printf("  wiring     : %s\n", WIRING_NAME);
    Serial.printf("  I2S        : BCLK=%d LRC=%d DIN=%d  SD=%d GAIN=%d\n",
                  PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DIN, PIN_AMP_SD, PIN_AMP_GAIN);
    Serial.printf("  LittleFS   : %u / %u bytes used\n",
                  (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
    Serial.printf("  stream buf : %u KB in %s\n", STREAM_BUF_BYTES / 1024,
                  ESP.getPsramSize() ? "PSRAM" : "internal RAM");
    Serial.printf("  free heap  : %u bytes\n", ESP.getFreeHeap());
    Serial.printf("  AP         : %s / %s -> http://%s/\n",
                  AP_SSID, AP_PASS, WiFi.softAPIP().toString().c_str());
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("  STA        : %s -> http://%s/  (http://" HOSTNAME ".local/)\n",
                      WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    } else {
        Serial.println("  STA        : not connected — join the AP and set Wi-Fi");
    }

    // Auto-resume is a trap without a guard: if the saved station crashes the
    // decoder, the device reboots, resumes it again, and is bricked in a loop
    // with no way in. Count boots that never reached stable playback; after
    // three, forget the station and come up idle so the user can always get back.
    int bootTry = prefs.getInt("bootTry", 0) + 1;
    prefs.putInt("bootTry", bootTry);
    if (bootTry >= 3) {
        Serial.printf("[boot] %d starts without stable playback — forgetting the "
                      "saved station so we can boot clean\n", bootTry);
        prefs.remove("lastUrl");
        prefs.remove("lastName");
        prefs.putInt("bootTry", 0);
    }

    // Pick up where we left off: last station if we have internet. With no
    // network the puck has no screen and no sticker, so the only way a first-time
    // user can learn the access point's name and password is to be told them.
    String last = prefs.getString("lastUrl", "");
    if (WiFi.status() == WL_CONNECTED) {
        if (last.length()) {
            post(CMD_STREAM, last.c_str(), prefs.getString("lastName", "").c_str());
        } else {
            post(CMD_STREAM, DEFAULT_STATION_URL, DEFAULT_STATION_NAME);
        }
    } else {
        post(CMD_SAY, "Hello! I am new sheen. " SETUP_SPEECH
                      " Press my button any time to hear that again.");
    }
}

// --------------------------------------------------------------------- loop
// ------------------------------------------------------------------ buttons
//
// SW3 (the user button) carries four gestures:
//   1 press  - mute / unmute        2 presses - next favourite
//   3 presses - random station      hold      - volume ramp, release to set
// SW2 (BOOT) held 5 s enters setup mode. Destructive actions live on the
// awkward button on purpose: forgetting the network should never be something
// you do by fumbling the button you use every day.
static void applyMute() {
    ampEnable(state != IDLE);          // re-evaluates the muted flag internally
    Serial.printf("[btn] %s\n", muted ? "muted" : "unmuted");
}

static void nextFavourite() {
    JsonArray a = favDoc.as<JsonArray>();
    if (!a.size()) {
        post(CMD_SING);
        return;
    }
    static size_t idx = 0;
    if (state == STREAMING) {
        idx = (idx + 1) % a.size();
    }
    post(CMD_STREAM, a[idx]["url"] | "", a[idx]["name"] | "");
}

static uint32_t confirmResetUntil;   // set by SW2, consumed by SW3

static void serviceButton() {
    static bool down = false;
    static uint32_t downAt = 0, lastUp = 0;
    static int presses = 0;
    static bool ramping = false, rampUp = true;
    static float rampVol = 0.5f;

    bool now_down = (digitalRead(PIN_BUTTON) == LOW);
    uint32_t now = millis();

    // ---- press begins
    if (now_down && !down) {
        down = true;
        downAt = now;
    }

    // ---- held: after LONG_PRESS_MS this becomes a volume ramp
    if (now_down && down && (now - downAt) > LONG_PRESS_MS) {
        if (!ramping) {
            ramping = true;
            presses = 0;               // a hold is not a press
            rampVol = volume;
            rampUp = (volume < 0.95f); // start in the direction with headroom
        }
        static uint32_t lastStep = 0;
        if (!lastStep || (now - lastStep) > 250) {
            lastStep = now;            // fresh hold: don't integrate the gap
        }
        float dt = (now - lastStep) / 1000.0f;
        lastStep = now;
        rampVol += (rampUp ? 1.0f : -1.0f) * VOL_RAMP_PER_S * dt;
        if (rampVol >= 1.0f) {
            rampVol = 1.0f;
            rampUp = false;            // bounce, so one long hold sweeps both ways
        } else if (rampVol <= 0.0f) {
            rampVol = 0.0f;
            rampUp = true;
        }
        volume = rampVol;
        btnVolPreview = rampVol;
        // Pitch rises with the setting too: on a small speaker a very quiet tone
        // and silence are hard to tell apart, but the pitch still reads.
        toneHz = 300.0f + rampVol * 520.0f;
        toneWanted = (state == IDLE || gen == genTone);
        if (out) {
            out->SetGain(gainFor(volume));
        }
        return;
    }

    // ---- release
    if (!now_down && down) {
        down = false;
        uint32_t held = now - downAt;
        if (ramping) {
            ramping = false;
            btnVolPreview = -1.0f;
            rampUp = true;
            toneWanted = false;
            prefs.putFloat("vol", volume);
            Serial.printf("[btn] volume set to %.0f%%\n", volume * 100);
        } else if (held >= 30) {       // ignore contact bounce
            presses++;
            lastUp = now;
        }
    }

    // ---- decide once the multi-press window closes
    if (presses && !down && (now - lastUp) > MULTI_PRESS_MS) {
        int n = presses;
        presses = 0;
        if (confirmResetUntil) {
            confirmResetUntil = 0;
            Serial.println("[btn] reset confirmed — forgetting wifi");
            prefs.remove("ssid");
            prefs.remove("pass");
            prefs.putInt("bootTry", 0);
            post(CMD_SAY, "Forgetting the network. " SETUP_SPEECH);
            delay(7000);
            ESP.restart();
        } else if (WiFi.status() != WL_CONNECTED) {
            post(CMD_SAY, SETUP_SPEECH);   // offline: any tap repeats the way in
        } else if (n == 1) {
            muted = !muted;
            applyMute();
        } else if (n == 2) {
            nextFavourite();
        } else {
            if (!tuneRandomStation()) {
                post(CMD_SAY, "I could not find a station just now.");
            }
        }
    }
}

// SW2 is the BOOT strapping pin, which is ALSO driven by the USB host's DTR
// line. A serial monitor that asserts DTR holds it low for the whole session, so
// a bare "held 5 s" rule would let a USB cable wipe the user's network. The
// gesture therefore only *arms* the reset; confirming it requires SW3, which
// nothing on the USB side can press. Accidental DTR holds simply expire.
static void serviceBootButton() {
    static bool armed = false;
    static bool down = false;
    static uint32_t downAt = 0;

    // Ignore SW2 entirely for the first half minute: flashing and reset
    // sequences drive this pin, and boot is exactly when they happen.
    if (millis() < 30000) {
        return;
    }

    bool low = (digitalRead(PIN_BOOT_BUTTON) == LOW);
    uint32_t now = millis();

    if (confirmResetUntil && now > confirmResetUntil) {
        confirmResetUntil = 0;
        Serial.println("[boot-btn] reset not confirmed — cancelled");
    }

    if (!low) {
        armed = true;                  // seen released: a real press can follow
        down = false;
        bootHeldMs = 0;
        return;
    }
    if (!armed) {
        return;                        // low since we started watching
    }
    if (!down) {
        down = true;
        downAt = now;
    }
    bootHeldMs = now - downAt;
    if (bootHeldMs >= BOOT_RESET_MS) {
        bootHeldMs = 0;
        armed = false;
        down = false;
        confirmResetUntil = now + 12000;
        Serial.println("[boot-btn] armed — press the round button to confirm reset");
        post(CMD_SAY, "To forget the network, press my round button now.");
    }
}

// ---------------------------------------------------------------- serial CLI
//
// Everything the web UI can do, over USB-CDC. This is the bench handle: it makes
// the device testable without a phone on its access point, which is otherwise
// the only way in. Credentials are taken as arguments and stored in NVS — never
// compiled in, so they never reach the repo.
static void printStatus() {
    static const char *names[] = {"idle", "speaking", "singing", "file", "streaming"};
    Serial.printf("state      : %s\n", names[state]);
    Serial.printf("station    : %s\n", npStation);
    Serial.printf("title      : %s\n", npTitle);
    Serial.printf("url        : %s\n", curUrl);
    Serial.printf("effect     : %d (%s)  bri=%d speed=%d\n", fxState.effect,
                  EFFECT_NAMES[fxState.effect], fxState.brightness, fxState.speed);
    Serial.printf("volume     : %.2f\n", volume);
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("wifi       : %s  ip=%s  rssi=%d  ch=%d\n", WiFi.SSID().c_str(),
                      WiFi.localIP().toString().c_str(), WiFi.RSSI(), WiFi.channel());
    } else {
        Serial.printf("wifi       : not connected (phase %d)\n", (int)netPhase);
    }
    Serial.printf("heap       : %u free, %u min ever\n", ESP.getFreeHeap(), ESP.getMinFreeHeap());
    Serial.printf("psram      : %u free of %u\n", ESP.getFreePsram(), ESP.getPsramSize());
    if (state == STREAMING) {
        Serial.printf("ring       : %u / %u bytes%s\n", streamRing.available(),
                      streamRing.size(), streamRing.atEof() ? "  [source ended]" : "");
        if (srcNet) {
            Serial.printf("icy        : %u blocks, %u desyncs\n",
                          srcNet->icyBlocks, srcNet->icyDesyncs);
        }
        Serial.printf("net in     : %.1f KB/s (%u B total)\n",
                      streamRing.inRate() / 1024.0f, streamRing.inTotal());
        Serial.printf("dec out    : %.1f KB/s (%u B total)\n",
                      streamRing.outRate() / 1024.0f, streamRing.outTotal());
    } else if (srcBuf) {
        Serial.printf("buffer     : %u / %u bytes\n", srcBuf->getFillLevel(), STREAM_BUF_BYTES);
    }
}

static void handleLine(String line) {
    line.trim();
    if (!line.length()) {
        return;
    }
    int sp = line.indexOf(' ');
    String cmd = (sp < 0) ? line : line.substring(0, sp);
    String arg = (sp < 0) ? "" : line.substring(sp + 1);
    cmd.toLowerCase();

    if (cmd == "help") {
        Serial.println("wifi <ssid>|<pass>   join a network (stored in NVS)\n"
                       "scan                 list visible networks\n"
                       "search <query>       query the station directory\n"
                       "tune <url>           play a stream\n"
                       "play <file>          play an MP3 from LittleFS\n"
                       "say <text> | sing | stop\n"
                       "fx <0-10> | bri <1-255> | speed <0-255> | vol <0-100>\n"
                       "press <1|2|3>        mute / next favourite / random station\n"
                       "mute [0|1]           toggle or set mute\n"
                       "status | stats | files | favs");
    } else if (cmd == "wifi") {
        int bar = arg.indexOf('|');     // '|' not ' ': SSIDs may contain spaces
        if (bar < 0) {
            Serial.println("usage: wifi <ssid>|<password>");
            return;
        }
        String ssid = arg.substring(0, bar), pass = arg.substring(bar + 1);
        ssid.trim();
        prefs.putString("ssid", ssid);
        prefs.putString("pass", pass);
        Serial.printf("[cli] saved '%s' (%d-char password), connecting…\n",
                      ssid.c_str(), pass.length());
        connectSta(ssid, pass);
        printStatus();
    } else if (cmd == "scan") {
        int n = WiFi.scanNetworks();
        for (int i = 0; i < n; i++) {
            Serial.printf("  %-32s %4d dBm  ch%-3d %s\n", WiFi.SSID(i).c_str(), WiFi.RSSI(i),
                          WiFi.channel(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "open" : "locked");
        }
        WiFi.scanDelete();
    } else if (cmd == "search") {
        String j = searchStations(arg);
        JsonDocument d;
        deserializeJson(d, j);
        int i = 0;
        for (JsonObject s : d.as<JsonArray>()) {
            Serial.printf("  [%2d] %-34s %-5s %4dk  %s\n", i++, (const char *)(s["name"] | "?"),
                          (const char *)(s["codec"] | "?"), (int)(s["bitrate"] | 0),
                          (const char *)(s["url"] | ""));
        }
        if (!i) {
            Serial.println("  (no results)");
        }
    } else if (cmd == "tune") {
        post(CMD_STREAM, arg.c_str(), "CLI");
    } else if (cmd == "play") {
        post(CMD_FILE, arg.c_str());
    } else if (cmd == "say") {
        post(CMD_SAY, arg.c_str());
    } else if (cmd == "sing") {
        post(CMD_SING);
    } else if (cmd == "stop") {
        post(CMD_STOP);
    } else if (cmd == "netkill") {
        // Drop the station link for N seconds to prove ride-through and clean
        // reconnect. This is the one bench test the buffer exists for and the
        // only way to exercise the backoff path deliberately.
        int secs = arg.toInt() > 0 ? arg.toInt() : 15;
        Serial.printf("[cli] dropping wifi for %d s (ring has %u bytes)\n",
                      secs, streamRing.available());
        netkillUntil = millis() + (uint32_t)secs * 1000;
        WiFi.disconnect(false, false);
    } else if (cmd == "press") {
        // Bench equivalents of the physical gestures.
        int n = arg.toInt();
        if (n == 1) {
            muted = !muted;
            applyMute();
        } else if (n == 2) {
            nextFavourite();
        } else if (n == 3) {
            if (!tuneRandomStation()) {
                Serial.println("[cli] no playable random station found");
            }
        } else {
            Serial.println("usage: press 1|2|3   (mute / next favourite / random)");
        }
    } else if (cmd == "ramp") {
        // Bench equivalent of holding the button: sweep for N seconds.
        float secs = arg.toFloat() > 0 ? arg.toFloat() : 4.0f;
        Serial.printf("[cli] volume sweep for %.1f s\n", secs);
        uint32_t end = millis() + (uint32_t)(secs * 1000);
        bool up = volume < 0.95f;
        uint32_t last = millis();
        while (millis() < end) {
            uint32_t now = millis();
            float dt = (now - last) / 1000.0f;
            last = now;
            volume += (up ? 1.0f : -1.0f) * VOL_RAMP_PER_S * dt;
            if (volume >= 1.0f) { volume = 1.0f; up = false; }
            if (volume <= 0.0f) { volume = 0.0f; up = true; }
            btnVolPreview = volume;
            toneHz = 300.0f + volume * 520.0f;
            toneWanted = (state == IDLE || gen == genTone);
            if (out) {
                out->SetGain(gainFor(volume));
            }
            delay(20);
        }
        toneWanted = false;
        btnVolPreview = -1.0f;
        prefs.putFloat("vol", volume);
        Serial.printf("[cli] volume now %.0f%%\n", volume * 100);
    } else if (cmd == "mute") {
        muted = arg.length() ? (arg.toInt() != 0) : !muted;
        applyMute();
    } else if (cmd == "ap") {
        bool on = arg.toInt() != 0;
        if (on) {
            WiFi.softAP(AP_SSID, AP_PASS);
        } else {
            WiFi.softAPdisconnect(true);
        }
        Serial.printf("[cli] softAP %s\n", on ? "on" : "off");
    } else if (cmd == "tasks") {
        // Which tasks exist, what they are blocked on, and how much stack is
        // left. "state" is eRunning0/eReady1/eBlocked2/eSuspended3/eDeleted4.
        UBaseType_t n = uxTaskGetNumberOfTasks();
        TaskStatus_t *st = (TaskStatus_t *)malloc(n * sizeof(TaskStatus_t));
        if (st) {
            n = uxTaskGetSystemState(st, n, nullptr);
            for (UBaseType_t i = 0; i < n; i++) {
                Serial.printf("  %-16s prio=%-2u core=%-2d state=%d stack_free=%u\n",
                              st[i].pcTaskName, (unsigned)st[i].uxCurrentPriority,
                              (int)st[i].xCoreID > 1 ? -1 : (int)st[i].xCoreID,
                              (int)st[i].eCurrentState, (unsigned)st[i].usStackHighWaterMark);
            }
            free(st);
        }
    } else if (cmd == "icy") {
        NetStream::icyEnabled = arg.toInt() != 0;
        Serial.printf("[cli] ICY metadata %s\n", NetStream::icyEnabled ? "on" : "off");
    } else if (cmd == "fx") {
        fxState.effect = constrain(arg.toInt(), 0, FX_COUNT - 1);
        Serial.printf("[cli] effect %d = %s\n", fxState.effect, EFFECT_NAMES[fxState.effect]);
    } else if (cmd == "bri") {
        fxState.brightness = constrain(arg.toInt(), 1, 255);
    } else if (cmd == "speed") {
        fxState.speed = constrain(arg.toInt(), 0, 255);
    } else if (cmd == "vol") {
        volume = constrain(arg.toFloat() / 100.0f, 0.0f, 1.0f);
        if (out) {
            out->SetGain(gainFor(volume));
        }
    } else if (cmd == "files") {
        for (const String &f : listMp3s()) {
            Serial.printf("  %s\n", f.c_str());
        }
    } else if (cmd == "favs") {
        String s;
        serializeJson(favDoc, s);
        Serial.println(s);
    } else if (cmd == "status" || cmd == "stats") {
        printStatus();
    } else {
        Serial.printf("unknown command '%s' — try 'help'\n", cmd.c_str());
    }
}

static void serviceSerial() {
    static String line;
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            handleLine(line);
            line = "";
        } else if (line.length() < 400) {
            line += c;
        }
    }
}

// Once a stream has held up for a while, this boot counts as healthy.
static void clearBootGuard() {
    static bool cleared = false;
    static uint32_t streamingSince = 0;
    if (state != STREAMING) {
        streamingSince = 0;
        return;
    }
    if (!streamingSince) {
        streamingSince = millis();
    }
    if (!cleared && millis() - streamingSince > 20000) {
        prefs.putInt("bootTry", 0);
        cleared = true;
        Serial.println("[boot] playback stable — boot guard reset");
    }
}

void loop() {
    // If credentials exist but we are not associated, keep trying. A router
    // reboot or a slow AP must not leave the device offline until someone
    // power-cycles it.
    static uint32_t nextStaRetry = 0;
    if (!netkillUntil && WiFi.status() != WL_CONNECTED && netPhase != NET_JOINING &&
        millis() > nextStaRetry) {
        nextStaRetry = millis() + 30000;
        String ss = prefs.getString("ssid", "");
        if (ss.length()) {
            Serial.println("[wifi] still offline — retrying saved network");
            connectSta(ss, prefs.getString("pass", ""));
        }
    }
    if (netkillUntil && millis() > netkillUntil) {
        netkillUntil = 0;
        Serial.println("[cli] restoring wifi");
        WiFi.begin(prefs.getString("ssid", "").c_str(), prefs.getString("pass", "").c_str());
    }
    clearBootGuard();
    dns.processNextRequest();
    server.handleClient();
    serviceButton();
    serviceBootButton();
    serviceSerial();
    delay(2);
}
