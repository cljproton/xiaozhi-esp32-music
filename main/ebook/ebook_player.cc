#include "ebook_player.h"
#include "application.h"
#include "board.h"
#include "tts/tts_client.h"
#include "settings.h"
#include <esp_audio_simple_dec_default.h>

#include <algorithm>
#include <cstring>
#include <esp_log.h>
#include <tuple>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "EbookPlayer"

EbookPlayer& EbookPlayer::GetInstance() {
    static EbookPlayer instance;
    return instance;
}

EbookPlayer::EbookPlayer() = default;

// FNV-1a hash of the book URL -> short NVS key (8 hex chars, within the 15-char limit).
static std::string BookKey(const std::string& url) {
    uint32_t h = 2166136261u;
    for (unsigned char c : url) {
        h ^= c;
        h *= 16777619u;
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "%08lx", (unsigned long)h);
    return std::string(buf);
}

static int LoadProgress(const std::string& url) {
    Settings s("ebook", false);
    return s.GetInt(BookKey(url).c_str(), 0);
}

static void SaveProgress(const std::string& url, int paragraph) {
    Settings s("ebook", true);
    s.SetInt(BookKey(url).c_str(), paragraph);
}

bool EbookPlayer::Play(const std::string& url, const std::string& username,
                       const std::string& password) {
    if (active_) Stop();
    auto book = EbookLoad(url, username, password);
    if (!book.error.empty() || book.paragraphs.empty()) {
        ESP_LOGE(TAG, "Load failed: %s", book.error.c_str());
        return false;
    }
    book_ = std::move(book);
    book_url_ = url;
    username_ = username;
    password_ = password;
    int saved = LoadProgress(url);
    paragraph_ = (saved > 0 && static_cast<size_t>(saved) < book_.paragraphs.size())
                     ? static_cast<size_t>(saved) : 0;
    ESP_LOGI(TAG, "Resume '%s' from paragraph %u/%u", book_.name.c_str(),
             (unsigned)paragraph_, (unsigned)book_.paragraphs.size());
    active_ = true;
    paused_ = false;
    stopping_ = false;
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
    stream_data_.clear();
    if (synth_task_ != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(300));  // let the decode loop notice stopping_
    }
}

bool EbookPlayer::Pause() {
    if (!active_ || paused_) return false;
    paused_ = true;
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
    if (!book_url_.empty()) SaveProgress(book_url_, static_cast<int>(paragraph_));
    stopping_ = true;  // interrupt current decode loop; synth task advances
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

// Decode stream_data_ with esp_audio_simple_dec, using our own large output
// buffer. This avoids esp-gmf's fixed 512-byte decoder port that fails to
// grow for Edge TTS's MPEG2 24kHz frames. Decoded PCM goes to AudioService.
bool EbookPlayer::PlayStream() {
    if (stream_data_.empty()) return false;

    // Ensure simple decoders are registered (required before first open).
    static bool dec_registered = false;
    if (!dec_registered) {
        esp_audio_dec_register_default();
        esp_audio_simple_dec_register_default();
        dec_registered = true;
    }
    esp_audio_simple_dec_cfg_t cfg = {};
    cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    cfg.dec_cfg = nullptr;
    cfg.cfg_size = 0;
    cfg.use_frame_dec = false;  // let the built-in parser find MP3 frames
    esp_audio_simple_dec_handle_t dec = nullptr;
    esp_audio_err_t oerr = esp_audio_simple_dec_open(&cfg, &dec);
    if (oerr != ESP_AUDIO_ERR_OK || dec == nullptr) {
        ESP_LOGE(TAG, "simple dec open failed: %d", oerr);
        return false;
    }

    // 4096-byte PCM output buffer (large enough for any MP3 frame).
    const int kOutSize = 4096;
    auto* out_buf = static_cast<uint8_t*>(malloc(kOutSize));
    if (out_buf == nullptr) {
        esp_audio_simple_dec_close(dec);
        return false;
    }

    size_t offset = 0;
    uint32_t pcm_id = 0;
    int sample_rate = 24000, channels = 1;
    bool got_info = false;
    bool ok = true;
    // Accumulate decoded PCM into reasonably sized batches. Pushing one tiny
    // frame (576 samples) at a time starves the 2-slot playback queue and
    // causes audible crackling.
    constexpr size_t kBatchSamples = 12000;  // ~0.5s at 24kHz
    std::vector<int16_t> batch;
    batch.reserve(kBatchSamples);
    auto flush_batch = [&]() {
        if (batch.empty() || stopping_) return;
        auto& svc = Application::GetInstance().GetAudioService();
        svc.PushPcmPlaybackTask(std::move(batch), sample_rate, channels, ++pcm_id, 0);
        batch.clear();
        batch.reserve(kBatchSamples);
    };
    while (active_ && !stopping_ && offset < stream_data_.size()) {
        esp_audio_simple_dec_raw_t raw = {};
        raw.buffer = reinterpret_cast<uint8_t*>(const_cast<char*>(stream_data_.data() + offset));
        raw.len = stream_data_.size() - offset;
        raw.eos = (offset + raw.len >= stream_data_.size());
        esp_audio_simple_dec_out_t out = {};
        out.buffer = out_buf;
        out.len = kOutSize;
        esp_audio_err_t ret = esp_audio_simple_dec_process(dec, &raw, &out);
        offset += raw.consumed;
        if (raw.consumed == 0 && ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "decode no progress at %u (ret %d)", (unsigned)offset, ret);
            break;
        }
        if (ret != ESP_AUDIO_ERR_OK && ret != ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            ESP_LOGW(TAG, "decode failed ret %d", ret);
            ok = false;
            break;
        }
        if (out.decoded_size > 0 && !stopping_) {
            if (!got_info) {
                esp_audio_simple_dec_info_t info;
                if (esp_audio_simple_dec_get_info(dec, &info) == ESP_AUDIO_ERR_OK) {
                    sample_rate = info.sample_rate;
                    channels = info.channel;
                    got_info = true;
                }
            }
            auto* pcm = reinterpret_cast<const int16_t*>(out_buf);
            size_t samples = out.decoded_size / sizeof(int16_t);
            batch.insert(batch.end(), pcm, pcm + samples);
            if (batch.size() >= kBatchSamples) flush_batch();
        }
    }
    flush_batch();

    free(out_buf);
    esp_audio_simple_dec_close(dec);
    ESP_LOGI(TAG, "TTS decode done: %s (info=%d rate=%d ch=%d)", ok ? "ok" : "error",
             got_info, sample_rate, channels);
    return ok;
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
    active_ = true;
    stopping_ = false;
    bool ok = PlayStream();
    active_ = false;
    return ok;
}

void EbookPlayer::PlayTestAsync(const std::string& text, const std::string& voice, int speed) {
    auto params = std::make_unique<std::tuple<std::string, std::string, int>>(text, voice, speed);
    xTaskCreate([](void* arg) {
        auto* p = static_cast<std::tuple<std::string, std::string, int>*>(arg);
        auto owned = std::unique_ptr<std::tuple<std::string, std::string, int>>(p);
        EbookPlayer::GetInstance().PlayTest(std::get<0>(*owned), std::get<1>(*owned),
                                            std::get<2>(*owned));
        vTaskDelete(nullptr);
    }, "tts_test", 16384, params.release(), 2, nullptr);
}

void EbookPlayer::SynthTask(void* arg) {
    auto* self = static_cast<EbookPlayer*>(arg);
    while (self->active_ && !self->stopping_) {
        self->SynthesizeAndPlay();
        if (!self->active_ || self->stopping_) break;
        vTaskDelay(pdMS_TO_TICKS(200));
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
    stopping_ = false;
    if (!PlayStream()) {
        active_ = false;
        return;
    }
    ++paragraph_;
    if (!book_url_.empty()) SaveProgress(book_url_, static_cast<int>(paragraph_));
}
