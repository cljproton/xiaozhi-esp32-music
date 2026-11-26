#include "ebook_player.h"
#include "application.h"
#include "board.h"
#include "tts/tts_client.h"
#include "tts/edge_session.h"
#include "settings.h"
#include <esp_audio_simple_dec_default.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <esp_log.h>
#include <esp_system.h>
#include <tuple>
#include <utility>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "EbookPlayer"

namespace {

// FNV-1a hash of the book URL -> short NVS key (8 hex chars, within the 15-char limit).
std::string BookKey(const std::string& url) {
    uint32_t h = 2166136261u;
    for (unsigned char c : url) { h ^= c; h *= 16777619u; }
    char buf[16];
    snprintf(buf, sizeof(buf), "%08lx", (unsigned long)h);
    return std::string(buf);
}

// Progress = raw file offset of the current chunk + text offset inside it.
void LoadProgress(const std::string& url, size_t& raw, size_t& text) {
    Settings s("ebook", false);
    std::string k = BookKey(url);
    raw = static_cast<size_t>(s.GetInt(k.c_str(), 0));
    text = static_cast<size_t>(s.GetInt((k + "t").c_str(), 0));
}

void SaveProgress(const std::string& url, size_t raw, size_t text) {
    if (url.empty()) return;
    Settings s("ebook", true);
    std::string k = BookKey(url);
    s.SetInt(k.c_str(), static_cast<int>(raw));
    s.SetInt((k + "t").c_str(), static_cast<int>(text));
}

constexpr int kOutSize = 4096;
constexpr size_t kBatchSamples = 12000;  // 0.5s at 24kHz

// One MP3 decoder + PCM batching kept alive for a whole playback session, so
// consecutive ring reads continue the same decode stream.
struct Mp3Pipeline {
    esp_audio_simple_dec_handle_t dec = nullptr;
    uint8_t* out = nullptr;
    std::vector<int16_t> batch;
    uint32_t pcm_id = 0;
    int sample_rate = 24000;
    int channels = 1;
    bool have_info = false;
    size_t pcm_bytes = 0;
    const std::atomic<bool>* stopping = nullptr;

    bool Open(const std::atomic<bool>& stop) {
        stopping = &stop;
        static bool registered = false;
        if (!registered) {
            esp_audio_dec_register_default();
            esp_audio_simple_dec_register_default();
            registered = true;
        }
        esp_audio_simple_dec_cfg_t cfg = {};
        cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
        cfg.use_frame_dec = false;
        if (esp_audio_simple_dec_open(&cfg, &dec) != ESP_AUDIO_ERR_OK || dec == nullptr) {
            ESP_LOGE(TAG, "simple dec open failed");
            dec = nullptr;
            return false;
        }
        out = static_cast<uint8_t*>(malloc(kOutSize));
        if (out == nullptr) {
            esp_audio_simple_dec_close(dec);
            dec = nullptr;
            return false;
        }
        batch.reserve(kBatchSamples);
        return true;
    }

    void Flush() {
        if (batch.empty()) return;
        if (stopping == nullptr || !stopping->load()) {
            Application::GetInstance().GetAudioService().PushPcmPlaybackTask(
                std::move(batch), sample_rate, channels, ++pcm_id, 0);
        }
        batch.clear();
        batch.reserve(kBatchSamples);
    }

    void Close() {
        Flush();
        ESP_LOGI(TAG, "pipeline closed: %u PCM bytes, heap %u",
                 (unsigned)pcm_bytes, (unsigned)esp_get_free_heap_size());
        if (out != nullptr) free(out);
        out = nullptr;
        if (dec != nullptr) esp_audio_simple_dec_close(dec);
        dec = nullptr;
    }

    void Drain(EdgeSession& session) {
        uint8_t mp3[512];
        while (!(stopping != nullptr && stopping->load()) && session.AudioAvailable() > 0) {
            size_t n = session.ReadAudio(mp3, sizeof(mp3));
            if (n == 0) break;
            size_t off = 0;
            while (off < n) {
                esp_audio_simple_dec_raw_t raw = {};
                raw.buffer = mp3 + off;
                raw.len = static_cast<uint32_t>(n - off);
                esp_audio_simple_dec_out_t pcm_out = {};
                pcm_out.buffer = out;
                pcm_out.len = kOutSize;
                esp_audio_simple_dec_process(dec, &raw, &pcm_out);
                if (raw.consumed == 0 && pcm_out.decoded_size == 0) break;  // stuck
                off += raw.consumed;
                if (pcm_out.decoded_size == 0) continue;
                if (!have_info) {
                    esp_audio_simple_dec_info_t info;
                    if (esp_audio_simple_dec_get_info(dec, &info) == ESP_AUDIO_ERR_OK) {
                        sample_rate = info.sample_rate;
                        channels = info.channel;
                        have_info = true;
                    }
                }
                auto* pcm = reinterpret_cast<int16_t*>(out);
                pcm_bytes += pcm_out.decoded_size;
                batch.insert(batch.end(), pcm, pcm + pcm_out.decoded_size / sizeof(int16_t));
                if (batch.size() >= kBatchSamples) Flush();
            }
        }
    }
};

}  // namespace

EbookPlayer& EbookPlayer::GetInstance() {
    static EbookPlayer instance;
    return instance;
}

EbookPlayer::EbookPlayer() = default;

bool EbookPlayer::Play(const std::string& url, const std::string& username,
                       const std::string& password) {
    if (active_) Stop();
    size_t raw = 0, text = 0;
    LoadProgress(url, raw, text);
    if (!stream_.Open(url, username, password, raw, text)) {
        ESP_LOGE(TAG, "Open stream failed: %s", url.c_str());
        return false;
    }
    book_.name = url.substr(url.find_last_of('/') + 1);
    book_url_ = url;
    username_ = username;
    password_ = password;
    paragraph_ = 0;
    ESP_LOGI(TAG, "Streaming '%s' from chunk %u (+%u)", book_.name.c_str(),
             (unsigned)raw, (unsigned)text);
    active_ = true;
    paused_ = false;
    stopping_ = false;
    skip_requested_ = false;
    feed_done_ = false;
    if (synth_task_) {
        vTaskDelete(synth_task_);
        synth_task_ = nullptr;
    }
    // Decode/playback runs on its own task so a reconnect or a slow HTTP Range
    // fetch can never starve the I2S output.
    xTaskCreatePinnedToCore(PlayTask, "ebook_play", 12288, this, 4, &play_task_, 1);
    xTaskCreatePinnedToCore(SynthTask, "ebook_synth", 20480, this, 2, &synth_task_, 0);
    return true;
}

void EbookPlayer::Stop() {
    stopping_ = true;
    SaveProgress(book_url_, stream_.RawPosition(), stream_.TextPosition());
    vTaskDelay(pdMS_TO_TICKS(400));
    active_ = false;
    paused_ = false;
    EdgeSession::GetInstance().Disconnect();
    stream_.Close();
    stopping_ = false;
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
    if (!active_) return false;
    skip_requested_ = true;
    return true;
}

EbookPlayer::Status EbookPlayer::GetStatus() const {
    Status s;
    s.active = active_;
    s.paused = paused_;
    s.book_name = book_.name;
    s.paragraph = paragraph_;
    s.total = stream_.TotalBytes() > 0 ? stream_.TotalBytes() / 6 : 0;
    return s;
}

bool EbookPlayer::PlayTest(const std::string& text, const std::string& voice, int speed) {
    if (text.empty()) return false;
    auto& session = EdgeSession::GetInstance();
    if (!session.Connect(voice.empty() ? TtsGetVoice() : voice, speed)) {
        ESP_LOGE(TAG, "test connect failed");
        return false;
    }
    if (!session.Speak(text)) {
        session.Disconnect();
        return false;
    }
    active_ = true;
    stopping_ = false;
    Mp3Pipeline pipe;
    if (pipe.Open(stopping_)) {
        int waited = 0;
        while (!stopping_ && waited < 60000) {
            pipe.Drain(session);
            if (session.AudioAvailable() == 0 && session.Idle()) break;
            vTaskDelay(pdMS_TO_TICKS(20));
            waited += 20;
        }
        pipe.Close();
    }
    session.Disconnect();
    active_ = false;
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
    }, "tts_test", 20480, params.release(), 2, nullptr);
}

void EbookPlayer::SynthTask(void* arg) {
    auto* self = static_cast<EbookPlayer*>(arg);
    auto& session = EdgeSession::GetInstance();
    auto bail = [&]() {
        session.Disconnect();
        self->synth_task_ = nullptr;
        vTaskDelete(nullptr);
    };
    if (!session.Connect(TtsGetVoice(), TtsGetSpeed())) {
        ESP_LOGE(TAG, "EdgeSession connect failed");
        self->feed_done_ = true;
        bail();
    }

    // Sentences handed to the server but not yet acknowledged with turn.end,
    // with their send attempts. Kept so a dropped connection can resend them
    // instead of skipping text; the cap stops a flapping link from repeating
    // the same sentence forever.
    std::deque<std::pair<std::string, int>> inflight;
    int last_done = 0;

    // Two requests in flight keep the connection busy: Edge closes connections
    // that stay idle for ~35s, and the queued turn also hides server latency.
    constexpr int kMaxPending = 2;
    bool eof = false;
    int sentences = 0;
    while (self->active_ && !self->stopping_) {
        int done = session.DoneCount();
        while (last_done < done && !inflight.empty()) {
            inflight.pop_front();
            ++last_done;
        }

        if (self->skip_requested_.exchange(false)) {
            // Drop queued audio and resync; text already read stays consumed.
            inflight.clear();
            session.Reset();
            session.Disconnect();
            if (!session.Connect(TtsGetVoice(), TtsGetSpeed())) break;
            last_done = 0;
            continue;
        }

        if (!session.IsConnected()) {
            // Reconnect immediately; buffered audio keeps playing meanwhile.
            ESP_LOGW(TAG, "reconnecting (%u in flight)", (unsigned)inflight.size());
            session.ResetCounters();
            session.Disconnect();
            if (!session.Connect(TtsGetVoice(), TtsGetSpeed())) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            last_done = 0;
            std::deque<std::pair<std::string, int>> retry;
            retry.swap(inflight);
            for (auto& s : retry) {
                if (s.second >= 3) {  // flapping link: stop repeating this line
                    ESP_LOGW(TAG, "giving up on a sentence after %d tries", s.second);
                    continue;
                }
                if (!session.Speak(s.first)) break;
                inflight.emplace_back(s.first, s.second + 1);
            }
            continue;
        }

        while (!eof && self->active_ && !self->stopping_ &&
               session.PendingCount() < kMaxPending &&
               session.AudioAvailable() < 384 * 1024) {
            std::string sentence;
            if (!self->stream_.NextSentence(sentence)) {
                eof = self->stream_.EndOfFile();
                if (!eof) vTaskDelay(pdMS_TO_TICKS(500));  // transient HTTP error
                break;
            }
            if (sentence.empty()) continue;
            if (!session.Speak(sentence)) {
                inflight.emplace_back(sentence, 1);
                session.Disconnect();  // force the reconnect path to resend
                break;
            }
            inflight.emplace_back(sentence, 1);
            ++self->paragraph_;
            if (++sentences % 10 == 0) {
                SaveProgress(self->book_url_, self->stream_.RawPosition(),
                             self->stream_.TextPosition());
            }
        }
        if (eof && inflight.empty() && session.Idle()) {
            self->feed_done_ = true;  // player drains what is left in the ring
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    SaveProgress(self->book_url_, self->stream_.RawPosition(), self->stream_.TextPosition());
    bail();
}

void EbookPlayer::PlayTask(void* arg) {
    auto* self = static_cast<EbookPlayer*>(arg);
    auto& session = EdgeSession::GetInstance();
    Mp3Pipeline pipe;
    if (pipe.Open(self->stopping_)) {
        while (!self->stopping_) {
            pipe.Drain(session);
            if (self->feed_done_ && session.AudioAvailable() == 0) break;
            if (!self->active_ && session.AudioAvailable() == 0) break;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        pipe.Close();
    }
    if (self->feed_done_ && !self->stopping_) {
        SaveProgress(self->book_url_, 0, 0);  // finished: start over next time
        ESP_LOGI(TAG, "Book finished");
    }
    self->active_ = false;
    self->play_task_ = nullptr;
    vTaskDelete(nullptr);
}
