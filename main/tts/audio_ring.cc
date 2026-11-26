#include "audio_ring.h"
#include <esp_heap_caps.h>
#include <freertos/task.h>

AudioRing::AudioRing(size_t capacity) : cap_(capacity), head_(0), tail_(0) {
    buf_ = static_cast<uint8_t*>(heap_caps_malloc(cap_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buf_ == nullptr) {
        buf_ = static_cast<uint8_t*>(heap_caps_malloc(cap_, MALLOC_CAP_8BIT));
    }
    mutex_ = xSemaphoreCreateMutex();
}

AudioRing::~AudioRing() {
    if (buf_) heap_caps_free(buf_);
    if (mutex_) vSemaphoreDelete(mutex_);
}

size_t AudioRing::Write(const uint8_t* data, size_t len) {
    if (buf_ == nullptr || mutex_ == nullptr) return 0;
    // Backpressure instead of dropping: losing any bytes mid-sentence makes a
    // whole passage sound skipped. Wait briefly for the player to drain.
    int waited_ms = 0;
    while (true) {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        size_t free_sp = cap_ - (tail_ - head_);
        if (free_sp > 0 || waited_ms >= 1000) {
            size_t n = len < free_sp ? len : free_sp;
            for (size_t i = 0; i < n; ++i) {
                buf_[(tail_ + i) % cap_] = data[i];
            }
            tail_ += n;
            xSemaphoreGive(mutex_);
            return n;
        }
        xSemaphoreGive(mutex_);
        vTaskDelay(pdMS_TO_TICKS(10));
        waited_ms += 10;
    }
}

size_t AudioRing::Read(uint8_t* out, size_t max) {
    if (buf_ == nullptr || mutex_ == nullptr) return 0;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    size_t avail = tail_ - head_;
    size_t n = max < avail ? max : avail;
    for (size_t i = 0; i < n; ++i) {
        out[i] = buf_[(head_ + i) % cap_];
    }
    head_ += n;
    xSemaphoreGive(mutex_);
    return n;
}

size_t AudioRing::Available() const {
    return tail_ - head_;
}

size_t AudioRing::Free() const {
    return cap_ - (tail_ - head_);
}

void AudioRing::Reset() {
    if (mutex_ == nullptr) return;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    head_ = tail_ = 0;
    xSemaphoreGive(mutex_);
}
