#pragma once

#include <cstddef>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// PSRAM-backed ring buffer for streaming MP3 bytes from the network task to
// the decode/play task. Fixed capacity; overwrite is disabled (writer blocks
// via a full flag checked by the caller).
class AudioRing {
public:
    explicit AudioRing(size_t capacity);
    ~AudioRing();

    size_t Write(const uint8_t* data, size_t len);  // returns bytes written
    size_t Read(uint8_t* out, size_t max);          // returns bytes read
    size_t Available() const;
    size_t Free() const;
    void Reset();

private:
    uint8_t* buf_;
    size_t cap_;
    size_t head_ = 0;  // read
    size_t tail_ = 0;  // write
    SemaphoreHandle_t mutex_;
};
