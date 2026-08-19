// StreamBuffer — a decoupled prefetch buffer for live streams.
//
// ESP8266Audio's AudioFileSourceBuffer cannot be used for internet radio. Its
// read() refills with a single *blocking* `src->read(buffer, buffSize)` whenever
// it runs dry, which assumes the source can deliver a whole buffer on demand.
// A live station is throttled to realtime: after the server's initial burst,
// bytes arrive at exactly the bitrate, so that call can never be satisfied. It
// stalls until the socket timeout, returns a partial buffer, and the decoder is
// left holding a torn frame — which shows up as 'bad main_data_begin pointer'
// and a click every few seconds.
//
// So: a producer task pulls from the network as fast as the station will send,
// into a FreeRTOS stream buffer living in PSRAM, and the decoder consumes from
// the other end. Network jitter is absorbed by the ring instead of being felt in
// the decode path. Single producer, single consumer — which is exactly the
// contract a FreeRTOS stream buffer is built for, so no locking of our own.

#pragma once
#include <Arduino.h>
#include <AudioFileSource.h>
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>

// Which core the producer runs on. The reference radios put network and decode
// together and UI on the other core; this build splits them. Flippable so the
// difference can be measured rather than argued about.
#ifndef NETFILL_CORE
#define NETFILL_CORE 0
#endif

class StreamBuffer : public AudioFileSource {
public:
    // storage must be at least size + 1 bytes and outlive this object.
    bool init(uint8_t *storage, size_t size) {
        capacity = size;
        sb = xStreamBufferCreateStatic(size, 1, storage, &sbStatic);
        return sb != nullptr;
    }

    // Begin pulling from `source`. Does not take ownership.
    bool start(AudioFileSource *source) {
        stop();
        src = source;
        xStreamBufferReset(sb);
        eof = false;
        running = true;
        inBytes = outBytes = consumed = 0;
        startMs = millis();
        // 12 KB, not the 4 KB you would guess from what this task appears to do.
        // A socket read is not a shallow call: NetworkClient::read() runs
        // lwip_recv, which acknowledges the received data inline, which drives
        // tcp_output -> ip4_output -> ethernet_output -> esp_wifi_internal_tx
        // -> ieee80211_output_do, all on the *calling* task's stack. Measured at
        // 38 frames deep. At 4 KB this tripped the stack canary and panicked the
        // whole device a minute or two into every stream.
        return xTaskCreatePinnedToCore(trampoline, "netfill", 12288, this, 3, &task, NETFILL_CORE) == pdPASS;
    }

    void stop() {
        if (!task) {
            return;
        }
        running = false;
        // The producer may be parked in xStreamBufferSend waiting for room, so
        // drain the ring until it notices and exits.
        for (int i = 0; i < 200 && task; i++) {
            xStreamBufferReset(sb);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        src = nullptr;
    }

    // Hold until the ring has a cushion, so playback starts with something to
    // spend. Returns false if the stream died before reaching it.
    bool prebuffer(size_t bytes, uint32_t timeoutMs) {
        uint32_t deadline = millis() + timeoutMs;
        while (millis() < deadline) {
            if (available() >= bytes) {
                return true;
            }
            if (eof) {
                return available() > 0;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        return available() > 0;
    }

    // Throughput accounting, so a starving stream can be told apart from a
    // decoder that is over-consuming — they look identical from the fill level.
    float inRate() const {
        uint32_t ms = millis() - startMs;
        return ms ? (inBytes * 1000.0f / ms) : 0.0f;
    }
    float outRate() const {
        uint32_t ms = millis() - startMs;
        return ms ? (outBytes * 1000.0f / ms) : 0.0f;
    }
    uint32_t inTotal() const { return inBytes; }
    uint32_t outTotal() const { return outBytes; }

    size_t available() const {
        return sb ? xStreamBufferBytesAvailable(sb) : 0;
    }
    size_t size() const {
        return capacity;
    }
    bool atEof() const {
        return eof;
    }

    // ------------------------------------------------------- AudioFileSource
    uint32_t read(void *data, uint32_t len) override {
        uint8_t *p = (uint8_t *)data;
        uint32_t got = 0;
        uint32_t deadline = millis() + 5000;
        while (got < len) {
            uint32_t n = xStreamBufferReceive(sb, p + got, len - got, pdMS_TO_TICKS(50));
            got += n;
            outBytes += n;
            if (got >= len) {
                break;
            }
            if (eof && available() == 0) {
                break;              // producer finished and the ring is drained
            }
            if (millis() > deadline) {
                break;              // never wedge the decoder permanently
            }
        }
        return got;
    }

    uint32_t readNonBlock(void *data, uint32_t len) override {
        uint32_t n = xStreamBufferReceive(sb, data, len, 0);
        outBytes += n;
        return n;
    }

    bool seek(int32_t, int) override {
        return false;
    }
    bool close() override {
        stop();
        return true;
    }
    bool isOpen() override {
        return running || available() > 0;
    }
    uint32_t getSize() override {
        return 0;
    }
    uint32_t getPos() override {
        return consumed;
    }
    bool loop() override {
        return true;                // filling happens in the producer task
    }

private:
    static void trampoline(void *self) {
        ((StreamBuffer *)self)->producer();
    }

    void producer() {
        uint8_t chunk[1460];        // one TCP segment's worth
        while (running) {
            if (!src) {
                break;
            }
            // Non-blocking: take whatever the socket has right now. Demanding a
            // fixed quantum makes every request wait on the tail of a second TCP
            // segment, which halves the achievable rate on a realtime-paced
            // stream. A producer should never impose a block size.
            uint32_t n = src->readNonBlock(chunk, sizeof(chunk));
            if (n == 0) {
                if (!src->isOpen()) {
                    break;          // socket closed by the station
                }
                vTaskDelay(1);      // nothing yet; yield a single tick
                continue;
            }
            consumed += n;
            inBytes += n;
            size_t off = 0;
            while (off < n && running) {
                // Bounded wait so `running` is still checked when the ring is
                // full — a portMAX_DELAY here would make stop() hang.
                off += xStreamBufferSend(sb, chunk + off, n - off, pdMS_TO_TICKS(200));
            }
        }
        eof = true;
        running = false;
        task = nullptr;
        vTaskDelete(nullptr);
    }

    StreamBufferHandle_t sb = nullptr;
    StaticStreamBuffer_t sbStatic;
    AudioFileSource *src = nullptr;
    TaskHandle_t task = nullptr;
    volatile bool running = false;
    volatile bool eof = false;
    size_t capacity = 0;
    uint32_t consumed = 0;
    volatile uint32_t inBytes = 0, outBytes = 0;
    uint32_t startMs = 0;
};
