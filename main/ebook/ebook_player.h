#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "ebook_reader.h"
#include "text_stream.h"
#include <esp_audio_simple_dec.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class EbookPlayer {
public:
    static EbookPlayer& GetInstance();

    bool Play(const std::string& url, const std::string& username = "",
              const std::string& password = "");
    void Stop();
    bool Pause();
    bool Resume();
    bool Next();
    bool IsPlaying() const { return active_; }
    bool IsPaused() const { return paused_; }

    struct Status {
        bool active = false;
        bool paused = false;
        std::string book_name;
        size_t paragraph = 0;
        size_t total = 0;
    };
    Status GetStatus() const;

    void SynthesizeAndPlay();
    bool PlayTest(const std::string& text, const std::string& voice, int speed);
    void PlayTestAsync(const std::string& text, const std::string& voice, int speed);
    static void SynthTask(void* arg);

private:
    EbookPlayer();
    // Play stream_data_ via esp_audio_simple_player (raw://). Blocks until
    // playback finishes or stops.
    bool PlayStream();

    std::atomic<bool> active_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> stopping_{false};
    EbookBook book_;
    TextStream stream_;
    std::string book_url_;
    size_t paragraph_ = 0;
    size_t book_offset_ = 0;  // saved byte offset for resume
    std::string username_;
    std::string password_;
    std::string stream_data_;
    TaskHandle_t synth_task_ = nullptr;
};
