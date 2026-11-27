#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "ebook_reader.h"
#include "text_stream.h"
#include "tts/audio_ring.h"
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
    bool Replay();
    bool Seek(int percent);
    bool IsPlaying() const { return active_; }
    bool IsPaused() const { return paused_; }

    struct Status {
        bool active = false;
        bool paused = false;
        std::string book_name;
        size_t paragraph = 0;
        size_t total = 0;
        bool error = false;
    };
    Status GetStatus() const;

    bool PlayTest(const std::string& text, const std::string& voice, int speed);
    void PlayTestAsync(const std::string& text, const std::string& voice, int speed);
    static void SynthTask(void* arg);
    static void PlayTask(void* arg);

private:
    EbookPlayer();

    std::atomic<bool> active_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> skip_requested_{false};
    std::atomic<bool> feed_done_{false};
    std::atomic<uint32_t> session_id_{0};  // 旧任务的残留 PCM 不能串进新一局
    bool finish_error_ = false;  // stream ended because of repeated HTTP errors
    AudioRing http_ring_{512 * 1024};
    void KeepOutputAlive();      // 暂停期间保持 I2S 输出常开，避免恢复时重配爆音
    void HttpSynthesis();        // HTTP provider 的逐句合成循环
    EbookBook book_;
    TextStream stream_;
    std::string book_url_;
    size_t paragraph_ = 0;  // sentences sent so far
    std::string username_;
    std::string password_;
    TaskHandle_t synth_task_ = nullptr;  // feeds text to TTS + reconnects
    TaskHandle_t play_task_ = nullptr;   // decodes ring -> PCM playback
};
