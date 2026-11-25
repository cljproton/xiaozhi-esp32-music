#include "ebook_player.h"
#include "application.h"
#include "board.h"
#include "tts/tts_client.h"

#include <esp_log.h>
#include <tuple>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "EbookPlayer"

EbookPlayer& EbookPlayer::GetInstance() {
    static EbookPlayer instance;
    return instance;
}

EbookPlayer::EbookPlayer() : player_(std::make_unique<NotifyPlayer>(Application::GetInstance().GetAudioService())) {}

bool EbookPlayer::Play(const std::string& url, const std::string& username,
                       const std::string& password) {
    if (active_) Stop();
    auto book = EbookLoad(url, username, password);
    if (!book.error.empty() || book.paragraphs.empty()) {
        ESP_LOGE(TAG, "Load failed: %s", book.error.c_str());
        return false;
    }
    book_ = std::move(book);
    username_ = username;
    password_ = password;
    paragraph_ = 0;
    active_ = true;
    paused_ = false;
    stopping_ = false;
    // Start a dedicated task for synthesis (never block the main loop).
    if (synth_task_) {
        vTaskDelete(synth_task_);
        synth_task_ = nullptr;
    }
    xTaskCreate(SynthTask, "ebook_synth", 10240, this, 2, &synth_task_);
    return true;
}

void EbookPlayer::Stop() {
    stopping_ = true;
    active_ = false;
    paused_ = false;
    player_->Stop();
    stream_data_.clear();
    if (synth_task_ != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(300));  // let the task notice stopping_
    }
}

bool EbookPlayer::Pause() {
    if (!active_ || paused_) return false;
    paused_ = true;
    player_->Stop();  // simplified pause: stop current paragraph
    return true;
}

bool EbookPlayer::Resume() {
    if (!active_ || !paused_) return false;
    paused_ = false;
    return true;
}

bool EbookPlayer::Next() {
    if (!active_ || paragraph_ + 1 >= book_.paragraphs.size()) return false;
    ++paragraph_;
    player_->Stop();
    return true;
}

EbookPlayer::Status EbookPlayer::GetStatus() const {
    Status s;
    s.active = active_;
    s.paused = paused_;
    s.book_name = book_.name;
    s.paragraph = paragraph_;
    s.total = book_.paragraphs.size();
    return s;
}

bool EbookPlayer::GetStreamChunk(size_t offset, const uint8_t** data, size_t* len) {
    if (offset >= stream_data_.size()) return false;
    *data = reinterpret_cast<const uint8_t*>(stream_data_.data() + offset);
    *len = stream_data_.size() - offset;
    return true;
}

void EbookPlayer::PlayTestAsync(const std::string& text, const std::string& voice, int speed) {
    auto params = std::make_unique<std::tuple<std::string, std::string, int>>(text, voice, speed);
    xTaskCreate([](void* arg) {
        auto* p = static_cast<std::tuple<std::string, std::string, int>*>(arg);
        auto owned = std::unique_ptr<std::tuple<std::string, std::string, int>>(p);
        EbookPlayer::GetInstance().PlayTest(std::get<0>(*owned), std::get<1>(*owned),
                                            std::get<2>(*owned));
        vTaskDelete(nullptr);
    }, "tts_test", 10240, params.release(), 2, nullptr);
}

bool EbookPlayer::PlayTest(const std::string& text, const std::string& voice, int speed) {
    if (text.empty()) return false;
    if (active_) Stop();
    stream_data_.clear();
    TtsRequest req;
    req.text = text;
    req.voice = voice.empty() ? TtsGetVoice() : voice;
    req.speed = speed;
    req.on_audio = [this](const uint8_t* data, size_t len) {
        stream_data_.append(reinterpret_cast<const char*>(data), len);
    };
    bool done = false;
    req.on_done = [&done](bool, const std::string&) { done = true; };
    if (!TtsSynthesize(req) || stream_data_.empty()) {
        ESP_LOGE(TAG, "TTS test failed");
        return false;
    }
    player_->Start("http://127.0.0.1/api/ebook/stream", {}, 1, nullptr, nullptr);
    return true;
}

void EbookPlayer::SynthTask(void* arg) {
    auto* self = static_cast<EbookPlayer*>(arg);
    while (self->active_ && !self->stopping_) {
        self->SynthesizeAndPlay();
        if (!self->active_ || self->stopping_) break;
        vTaskDelay(pdMS_TO_TICKS(200));  // brief gap between paragraphs
    }
    self->synth_task_ = nullptr;
    vTaskDelete(nullptr);
}

void EbookPlayer::SynthesizeAndPlay() {
    if (!active_ || stopping_ || paragraph_ >= book_.paragraphs.size()) {
        active_ = false;
        return;
    }
    stream_data_.clear();
    TtsRequest req;
    req.text = book_.paragraphs[paragraph_];
    req.voice = TtsGetVoice();
    req.speed = TtsGetSpeed();
    req.on_audio = [this](const uint8_t* data, size_t len) {
        stream_data_.append(reinterpret_cast<const char*>(data), len);
    };
    bool done = false;
    req.on_done = [&done](bool, const std::string&) { done = true; };
    bool ok = TtsSynthesize(req);
    if (!ok || stream_data_.empty()) {
        ESP_LOGE(TAG, "TTS failed for paragraph %u", (unsigned)paragraph_);
        active_ = false;
        return;
    }
    // Play the synthesized mp3 via the local HTTP stream endpoint. The
    // finished callback is invoked from the notify task; just mark completion
    // so the synthesis task advances to the next paragraph.
    player_->Start("http://127.0.0.1/api/ebook/stream", {}, 1, nullptr,
                   [this](uint32_t, bool) { paragraph_++; });
    // Wait for this paragraph to finish before synthesizing the next one.
    while (active_ && !stopping_ && player_->IsBusy()) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
