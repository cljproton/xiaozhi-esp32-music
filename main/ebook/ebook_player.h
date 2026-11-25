#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "ebook_reader.h"
#include "notify/notify_player.h"

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

    // The local HTTP stream endpoint calls this to serve the current mp3.
    // offset is the byte offset into stream_data_; returns false when done.
    bool GetStreamChunk(size_t offset, const uint8_t** data, size_t* len);
    size_t StreamSize() const { return stream_data_.size(); }
    void SynthesizeAndPlay();
    static void SynthTask(void* arg);

private:
    EbookPlayer();
    std::atomic<bool> active_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> stopping_{false};
    EbookBook book_;
    size_t paragraph_ = 0;
    std::string username_;
    std::string password_;
    std::string stream_data_;
    std::unique_ptr<NotifyPlayer> player_;
    TaskHandle_t synth_task_ = nullptr;
};
