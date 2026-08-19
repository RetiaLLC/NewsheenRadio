// NetStream — an ESP8266Audio file source that speaks HTTP and HTTPS, follows
// redirects, and strips Shoutcast/Icecast (ICY) metadata out of the audio.
//
// ESP8266Audio ships AudioFileSourceHTTPStream / AudioFileSourceICYStream, but
// neither works for a general internet radio on this board:
//   * their WiFiClient and HTTPClient members are private, so a subclass cannot
//     swap in a TLS client — and roughly half the stations in the directory are
//     https://, which would simply be unreachable;
//   * their open() disables redirect following on ESP32 (`#ifndef ESP32`), and
//     plenty of station URLs are a 301/302 away from the real endpoint.
//
// So this reimplements the small amount of HTTP a radio needs, over either a
// plain or a TLS client, with ICY parsing on top.
//
// TLS uses setInsecure(): no certificate validation. That is a deliberate
// trade-off for a public radio directory — the alternative is shipping and
// rotating a CA bundle for thousands of independent stations, and the payload
// is a public broadcast either way. Nothing secret is ever sent over it.

#pragma once
#include <Arduino.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <AudioFileSource.h>

class NetStream : public AudioFileSource {
public:
    NetStream() {}
    // Diagnostic knob: skip the Icy-MetaData request entirely. Lets the ICY path
    // be ruled in or out as a throughput suspect without changing anything else.
    static bool icyEnabled;
    ~NetStream() override {
        close();
    }

    enum { STATUS_CONNECTING = 2, STATUS_HTTPFAIL, STATUS_DISCONNECTED, STATUS_REDIRECT };

    bool open(const char *url) override {
        close();
        String target = url;
        for (int hop = 0; hop < 5; hop++) {
            String location;
            int code = request(target, location);
            if (code == 200) {
                pos = 0;
                streamUrl = target;
                return true;
            }
            if ((code == 301 || code == 302 || code == 303 || code == 307 || code == 308) &&
                location.length()) {
                cb.st(STATUS_REDIRECT, "redirect");
                closeSocket();
                target = absolute(target, location);
                continue;
            }
            cb.st(STATUS_HTTPFAIL, "HTTP request failed");
            closeSocket();
            return false;
        }
        cb.st(STATUS_HTTPFAIL, "too many redirects");
        closeSocket();
        return false;
    }

    uint32_t read(void *data, uint32_t len) override {
        return readInternal(data, len, false);
    }

    uint32_t readNonBlock(void *data, uint32_t len) override {
        return readInternal(data, len, true);
    }

    bool seek(int32_t, int) override {
        return false;                 // a live stream has nowhere to seek to
    }

    bool close() override {
        closeSocket();
        return true;
    }

    bool isOpen() override {
        return client && client->connected();
    }

    uint32_t getSize() override {
        return 0;                     // unbounded
    }

    uint32_t getPos() override {
        return pos;
    }

    const String &contentType() const {
        return mimeType;
    }
    const String &stationName() const {
        return icyName;
    }
    const String &resolvedUrl() const {
        return streamUrl;
    }

private:
    // ------------------------------------------------------------ URL helpers
    static bool split(const String &url, bool &tls, String &host, uint16_t &port, String &path) {
        int schemeEnd = url.indexOf("://");
        if (schemeEnd < 0) {
            return false;
        }
        String scheme = url.substring(0, schemeEnd);
        scheme.toLowerCase();
        tls = (scheme == "https");
        if (!tls && scheme != "http") {
            return false;
        }
        String rest = url.substring(schemeEnd + 3);
        int slash = rest.indexOf('/');
        String hostPort = (slash < 0) ? rest : rest.substring(0, slash);
        path = (slash < 0) ? "/" : rest.substring(slash);
        int colon = hostPort.lastIndexOf(':');
        // lastIndexOf is safe here: we already stripped the scheme, and a bare
        // IPv6 literal in a station URL is not a case worth carrying.
        if (colon > 0) {
            host = hostPort.substring(0, colon);
            port = hostPort.substring(colon + 1).toInt();
        } else {
            host = hostPort;
            port = tls ? 443 : 80;
        }
        return host.length() > 0;
    }

    // Resolve a Location header that may be relative.
    static String absolute(const String &base, const String &location) {
        if (location.startsWith("http://") || location.startsWith("https://")) {
            return location;
        }
        bool tls;
        String host, path;
        uint16_t port;
        if (!split(base, tls, host, port, path)) {
            return location;
        }
        String root = String(tls ? "https://" : "http://") + host;
        if ((tls && port != 443) || (!tls && port != 80)) {
            root += ":" + String(port);
        }
        if (location.startsWith("/")) {
            return root + location;
        }
        int lastSlash = path.lastIndexOf('/');
        return root + path.substring(0, lastSlash + 1) + location;
    }

    // ------------------------------------------------------------- connection
    int request(const String &url, String &location) {
        bool tls;
        String host, path;
        uint16_t port;
        if (!split(url, tls, host, port, path)) {
            return -1;
        }

        if (tls) {
            WiFiClientSecure *sc = new WiFiClientSecure();
            sc->setInsecure();          // see the header comment
            sc->setTimeout(8);
            client = sc;
        } else {
            client = new WiFiClient();
            client->setTimeout(8);
        }
        cb.st(STATUS_CONNECTING, "connecting");
        if (!client->connect(host.c_str(), port, 8000)) {
            return -1;
        }

        String req = "GET " + path + " HTTP/1.1\r\n";
        req += "Host: " + host + "\r\n";
        req += "User-Agent: Newsheen/1.0\r\n";
        if (icyEnabled) {
            req += "Icy-MetaData: 1\r\n";
        }
        req += "Accept: */*\r\n";
        req += "Connection: close\r\n\r\n";
        client->print(req);

        // ---- status line
        String line = readLine();
        if (!line.startsWith("HTTP/") && !line.startsWith("ICY")) {
            return -1;
        }
        int sp = line.indexOf(' ');
        int code = (sp > 0) ? line.substring(sp + 1, sp + 4).toInt() : -1;
        if (line.startsWith("ICY 200")) {
            code = 200;                 // Shoutcast v1 answers "ICY 200 OK"
        }

        // ---- headers
        icyMetaInt = 0;
        icyLeft = 0;
        chunked = false;
        chunkLeft = 0;
        chunkNeedHeader = true;
        chunkTrailer = 0;
        chunkHdr = "";
        mimeType = "";
        icyName = "";
        location = "";
        for (;;) {
            line = readLine();
            if (line.length() == 0) {
                break;
            }
            String lower = line;
            lower.toLowerCase();
            if (lower.startsWith("content-type:")) {
                mimeType = trimValue(line);
                mimeType.toLowerCase();
            } else if (lower.startsWith("icy-metaint:")) {
                icyMetaInt = trimValue(line).toInt();
                icyLeft = icyMetaInt;
            } else if (lower.startsWith("icy-name:")) {
                icyName = trimValue(line);
            } else if (lower.startsWith("location:")) {
                location = trimValue(line);
            } else if (lower.startsWith("transfer-encoding:") &&
                       lower.indexOf("chunked") >= 0) {
                // nginx-fronted stations serve HTTP/1.1 bodies chunked. The chunk
                // size lines are transport framing, not audio — feeding them to
                // the decoder corrupts it AND shifts the ICY metaint count, which
                // desyncs metadata permanently a block or two in.
                chunked = true;
            }
        }
        return code;
    }

    static String trimValue(const String &header) {
        int colon = header.indexOf(':');
        String v = header.substring(colon + 1);
        v.trim();
        return v;
    }

    String readLine() {
        String s;
        uint32_t deadline = millis() + 8000;
        while (millis() < deadline) {
            if (!client->connected() && !client->available()) {
                break;
            }
            int c = client->read();
            if (c < 0) {
                delay(1);
                continue;
            }
            if (c == '\n') {
                if (s.endsWith("\r")) {
                    s.remove(s.length() - 1);
                }
                return s;
            }
            s += (char)c;
            if (s.length() > 512) {
                break;              // no sane header is this long; bail
            }
        }
        return s;
    }

    void closeSocket() {
        if (client) {
            client->stop();
            delete client;
            client = nullptr;
        }
        icyMetaInt = 0;
        icyLeft = 0;
        icyMetaLeft = 0;
        icyHaveLen = false;
        chunked = false;
        chunkLeft = 0;
        chunkNeedHeader = true;
        chunkTrailer = 0;
        chunkHdr = "";
        bodyEnded = false;
    }

    // ------------------------------------------------------------------ body
    //
    // With ICY enabled the server splices a metadata block into the audio every
    // icyMetaInt bytes: one length byte (in 16-byte units) then that many bytes
    // of "StreamTitle='...';". Those bytes must never reach the decoder.
    //
    // This is a RESUMABLE state machine, and that is the whole point. The naive
    // version consumed the length byte and then demanded the whole block in one
    // go; if the socket ran dry mid-block it gave up *after* eating the length
    // byte, leaving the parser convinced it was still in audio. The next call
    // then read metadata content as a length byte and the stream desynced
    // permanently — every frame after that is garbage. Slow stations hit this
    // constantly (a 64 kbps stream delivers 8 KB/s, so a 4 KB metadata block is
    // simply not there yet), and it is invisible unless the station happens to
    // set the MP3 CRC-protection bit. Keeping the position across calls means a
    // half-arrived block is finished next time instead of losing framing.
    uint32_t readInternal(void *data, uint32_t len, bool nonBlock) {
        if (!client) {
            return 0;
        }
        uint8_t *out = (uint8_t *)data;
        uint32_t got = 0;
        uint32_t deadline = millis() + (nonBlock ? 0 : 1500);

        while (got < len) {
            if (icyMetaInt && icyLeft == 0) {
                if (!serviceMetadata()) {
                    break;              // block not fully arrived; state kept
                }
                icyLeft = icyMetaInt;
            }

            uint32_t want = len - got;
            if (icyMetaInt && want > (uint32_t)icyLeft) {
                want = icyLeft;
            }

            int n = rawRead(out + got, want);
            if (n <= 0) {
                if (bodyEnded || (!client->connected() && client->available() <= 0)) {
                    cb.st(STATUS_DISCONNECTED, "stream closed");
                    break;
                }
                if (nonBlock || millis() > deadline) {
                    break;
                }
                delay(1);
                continue;
            }
            got += n;
            pos += n;
            if (icyMetaInt) {
                icyLeft -= n;
            }
        }
        return got;
    }

    // Consume as much of the pending metadata block as has arrived. Returns true
    // only once the whole block is done; false means "call me again", with the
    // position preserved so framing survives.
    bool serviceMetadata() {
        if (!icyHaveLen) {
            uint8_t one;
            if (rawRead(&one, 1) != 1) {
                return false;
            }
            int c = one;
            icyHaveLen = true;
            icyMetaLeft = c * 16;
            icyMetaAccum = "";
            icyBlocks++;
            icyLastLen = c;
            if (icyMetaLeft == 0) {
                icyHaveLen = false;
                return true;            // the common case: nothing changed
            }
        }

        while (icyMetaLeft > 0) {
            uint8_t tmp[64];
            int chunk = icyMetaLeft < (int)sizeof(tmp) ? icyMetaLeft : (int)sizeof(tmp);
            int n = rawRead(tmp, chunk);
            if (n <= 0) {
                return false;           // resume on a later call
            }
            // Keep only a bounded prefix: titles are short, but a hostile or
            // broken server could claim 4080 bytes and we still must skip them all.
            if (icyMetaAccum.length() < 256) {
                for (int i = 0; i < n && icyMetaAccum.length() < 256; i++) {
                    icyMetaAccum += (char)(tmp[i] ? tmp[i] : ' ');
                }
            }
            icyMetaLeft -= n;
        }

        // Desync detector: a correctly framed block is either empty or begins
        // with StreamTitle. Anything else means we lost the audio/metadata
        // boundary and every byte after this is garbage to the decoder.
        if (icyMetaAccum.length() && icyMetaAccum.indexOf("StreamTitle") < 0) {
            icyDesyncs++;
            String prev = icyMetaAccum.substring(0, 24);
            Serial.printf("[icy] DESYNC block#%u len=%u first24=", icyBlocks, icyLastLen * 16);
            for (size_t i = 0; i < prev.length(); i++) {
                Serial.printf("%02x", (uint8_t)prev[i]);
            }
            Serial.println();
        }
        int start = icyMetaAccum.indexOf("StreamTitle='");
        if (start >= 0) {
            start += 13;
            int end = icyMetaAccum.indexOf("';", start);
            if (end < 0) {
                end = icyMetaAccum.length();
            }
            String title = icyMetaAccum.substring(start, end);
            title.trim();
            if (title.length() && title != lastTitle) {
                lastTitle = title;
                cb.md("StreamTitle", false, title.c_str());
            }
        }
        icyHaveLen = false;
        return true;
    }

    // Body reader: strips HTTP chunked framing when present, otherwise a plain
    // socket read. Never blocks — returns 0 when nothing has arrived yet, with
    // all framing state preserved so the next call resumes mid-chunk safely.
    int rawRead(uint8_t *buf, int want) {
        if (want <= 0 || !client) {
            return 0;
        }
        if (!chunked) {
            int avail = client->available();
            if (avail <= 0) {
                return 0;
            }
            if (avail < want) {
                want = avail;
            }
            int n = client->read(buf, want);
            return n > 0 ? n : 0;
        }
        if (bodyEnded) {
            return 0;
        }

        // Trailing CRLF closing the previous chunk.
        while (chunkTrailer > 0) {
            if (client->available() <= 0 || client->read() < 0) {
                return 0;
            }
            chunkTrailer--;
        }

        // Chunk header: "<hex size>[;ext]\r\n"
        while (chunkNeedHeader) {
            if (client->available() <= 0) {
                return 0;
            }
            int c = client->read();
            if (c < 0) {
                return 0;
            }
            if (c == '\n') {
                chunkLeft = strtol(chunkHdr.c_str(), nullptr, 16);
                chunkHdr = "";
                chunkNeedHeader = false;
                if (chunkLeft <= 0) {
                    bodyEnded = true;          // final zero-length chunk
                    return 0;
                }
            } else if (c != '\r' && chunkHdr.length() < 24) {
                chunkHdr += (char)c;
            }
        }

        int avail = client->available();
        if (avail <= 0) {
            return 0;
        }
        if (want > chunkLeft) {
            want = (int)chunkLeft;
        }
        if (want > avail) {
            want = avail;
        }
        int n = client->read(buf, want);
        if (n <= 0) {
            return 0;
        }
        chunkLeft -= n;
        if (chunkLeft == 0) {
            chunkTrailer = 2;                  // CRLF after the chunk data
            chunkNeedHeader = true;
        }
        return n;
    }

    WiFiClient *client = nullptr;      // WiFiClientSecure derives from this
    uint32_t pos = 0;
    int icyMetaInt = 0;
    int icyLeft = 0;
    int icyMetaLeft = 0;               // metadata bytes still to consume
    bool icyHaveLen = false;           // length byte for this block already read
    String icyMetaAccum;
    bool chunked = false;
    long chunkLeft = 0;
    bool chunkNeedHeader = true;
    int chunkTrailer = 0;
    bool bodyEnded = false;
    String chunkHdr;
    String mimeType, icyName, streamUrl, lastTitle;

public:
    uint32_t icyBlocks = 0, icyDesyncs = 0, icyLastLen = 0;
private:
};
