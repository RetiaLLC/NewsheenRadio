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
        if (!sb) {
            return false;
        }
        // One producer for the life of the device, parked on a notification
        // between streams.
        //
        // It used to be created per stream and self-delete on stop. A task that
        // deletes itself is reclaimed by the IDLE task of its core, and that
        // reclaim path does not fit in IDLE's 1536-byte stack once the task has
        // been through mbedTLS: the device panicked with "stack overflow in task
        // IDLE0" on the station-change path, intermittently, with a corrupted
        // TCB that made the reported task name garbage in about half the cases.
        // Never churning the task removes the reclaim entirely.
        //
        // 12 KB, not the 4 KB you would guess from what this task appears to do.
        // A socket read is not a shallow call: NetworkClient::read() runs
        // lwip_recv, which acknowledges the received data inline, which drives
        // tcp_output -> ip4_output -> ethernet_output -> esp_wifi_internal_tx
        // -> ieee80211_output_do, all on the *calling* task's stack. Measured at
        // 38 frames deep. At 4 KB this tripped the stack canary and panicked the
        // whole device a minute or two into every stream.
        return xTaskCreatePinnedToCore(trampoline, "netfill", 12288, this, 3, &task,
                                       NETFILL_CORE) == pdPASS;
    }

    // Begin pulling from `source`. Does not take ownership.
    bool start(AudioFileSource *source) {
        stop();
        if (alive) {
            // A previous producer never exited. Spawning a second one over the
            // same ring and src pointer is what corrupts memory; failing the
            // tune is recoverable, so refuse instead.
            return false;
        }
        if (!task) {
            return false;
        }
        src = source;
        xStreamBufferReset(sb);
        eof = false;
        running = true;
        // Set before waking the producer, so it can never clear it first.
        alive = true;
        inBytes = outBytes = consumed = 0;
        startMs = millis();
        xTaskNotifyGive(task);
        return true;
    }

    void stop() {
        if (!alive) {
            return;
        }
        running = false;
        // Wait for the producer to actually exit. The caller frees the source
        // the moment this returns, so returning early is a use-after-free: the
        // abandoned producer keeps calling src->readNonBlock() on freed memory,
        // and start() then spawns a second producer over the same ring whose
        // exit path nulls the new task's handle. The observed symptom is a
        // corrupted TCB reported as "stack overflow in task <garbage>" at the
        // next context switch, on the station-change path.
        //
        // The bound must clear the socket timeout, not just the ring wait. A
        // producer parked in the byte-at-a-time ICY metadata read blocks for up
        // to NetStream's 8 s read timeout, which a 2 s bound silently abandoned.
        // Do NOT reset the ring here to hurry the producer along. FreeRTOS
        // requires that a stream buffer not be reset while any task is sending
        // to or receiving from it, and the producer is doing exactly that until
        // it observes `running == false`. The old code reset it every 10 ms
        // through that window on every station change, which corrupted the
        // kernel's own lists -- the crash surfaced later and elsewhere, as a
        // LoadProhibited inside xTaskIncrementTick walking a garbage list node.
        //
        // No nudge is needed: the producer's send has a 200 ms timeout and
        // rechecks `running` each pass, so it exits on its own.
        const int kWaitMs = 20000;
        for (int i = 0; i < kWaitMs / 10 && alive; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (alive) {
            // Should be unreachable. Leave src pointing at live memory rather
            // than handing the producer a dangling pointer, and let start()
            // refuse the next stream instead of racing this one.
            abandoned++;
            return;
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
    // Nonzero means a producer outlived its stop() budget — the condition that
    // used to corrupt memory silently. Surfaced in `stats`.
    uint32_t abandonedProducers() const { return abandoned; }

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
        for (;;) {
            // Park with no stack committed until a stream needs pulling.
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            session();
            eof = true;
            running = false;
            // Last write of the session: this releases stop() to let the caller
            // free src, so nothing after it may touch src.
            alive = false;
        }
    }

    void session() {
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
    }

    StreamBufferHandle_t sb = nullptr;
    StaticStreamBuffer_t sbStatic;
    AudioFileSource *src = nullptr;
    TaskHandle_t task = nullptr;
    // The handshake that tells stop() the producer is done touching src. Set
    // before the task is created and cleared as its last act, so the ordering
    // holds no matter which core runs first.
    volatile bool alive = false;
    volatile bool running = false;
    volatile bool eof = false;
    size_t capacity = 0;
    uint32_t consumed = 0;
    volatile uint32_t inBytes = 0, outBytes = 0;
    uint32_t startMs = 0;
    volatile uint32_t abandoned = 0;
};
