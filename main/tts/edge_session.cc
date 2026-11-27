#include "edge_session.h"

#include <esp_log.h>
#include <esp_random.h>
#include <esp_crt_bundle.h>
#include <cstdio>
#include <algorithm>

#define TAG "EdgeSession"

namespace {
std::string MakeUuid4Hex() {
    uint8_t b[16];
    for (int i = 0; i < 16; ++i) b[i] = static_cast<uint8_t>(esp_random());
    b[6] = (b[6] & 0x0F) | 0x40;
    b[8] = (b[8] & 0x3F) | 0x80;
    static const char* H = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (int i = 0; i < 16; ++i) { out.push_back(H[b[i] >> 4]); out.push_back(H[b[i] & 0x0F]); }
    return out;
}

void XmlEscape(const std::string& in, std::string& out) {
    out.clear();
    for (char c : in) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            case '\n': case '\r': case '\t': out.push_back(' '); break;
            default: out.push_back(c);
        }
    }
}

std::string JsDateString() {
    time_t now = time(nullptr);
    struct tm tm_utc = {};
    gmtime_r(&now, &tm_utc);
    char buf[96];
    static const char* days[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char* mons[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                 "Jul","Aug","Sep","Oct","Nov","Dec"};
    snprintf(buf, sizeof(buf), "%s %s %02d %d %02d:%02d:%02d GMT+0000 (Coordinated Universal Time)",
             days[tm_utc.tm_wday % 7], mons[tm_utc.tm_mon % 12], tm_utc.tm_mday,
             tm_utc.tm_year + 1900, tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
    return buf;
}

std::string FullVoiceName(const std::string& voice) {
    size_t d1 = voice.find('-');
    if (d1 == std::string::npos) return voice;
    size_t d2 = voice.find('-', d1 + 1);
    if (d2 == std::string::npos) return voice;
    return "Microsoft Server Speech Text to Speech Voice (" + voice.substr(0, d2) + ", " +
           voice.substr(d2 + 1) + ")";
}
}  // namespace

EdgeSession& EdgeSession::GetInstance() {
    static EdgeSession instance;
    return instance;
}

EdgeSession& EdgeSession::GetTestInstance() {
    static EdgeSession instance;
    return instance;
}

void EdgeSession::EventHandler(void* arg, esp_event_base_t, int32_t event, void* data) {
    auto* self = static_cast<EdgeSession*>(arg);
    if (event == WEBSOCKET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "connected");
        self->connected_.store(true);
        return;
    }
    if (event == WEBSOCKET_EVENT_DATA) {
        auto* ev = static_cast<esp_websocket_event_data_t*>(data);
        if (ev->data_len <= 0) return;
        if (ev->op_code == WS_TRANSPORT_OPCODES_TEXT) {
            // turn.end marks the end of one queued sentence.
            std::string msg(ev->data_ptr, ev->data_len);
            if (msg.find("Path:turn.end") != std::string::npos) {
                int p = self->pending_.load();
                if (p > 0) self->pending_.store(p - 1);
                self->done_count_.store(self->done_count_.load() + 1);
            }
            return;
        }
        if (ev->op_code != WS_TRANSPORT_OPCODES_BINARY) return;
        // Frames larger than the WS buffer arrive as several events; only the
        // first one carries the text header (payload_offset == 0).
        if (ev->payload_offset > 0) {
            self->WriteAudio(reinterpret_cast<const uint8_t*>(ev->data_ptr),
                             static_cast<size_t>(ev->data_len));
            return;
        }
        const char* p = ev->data_ptr;
        int len = ev->data_len;
        static const char* kPath = "Path:audio\r\n";
        const int kPathLen = 12;
        for (int i = 0; i + kPathLen <= len; ++i) {
            if (p[i] != 'P' || memcmp(p + i, kPath, kPathLen) != 0) continue;
            int off = i + kPathLen;
            if (off + 1 < len && p[off] == '\r' && p[off + 1] == '\n') off += 2;
            if (off < len) {
                self->WriteAudio(reinterpret_cast<const uint8_t*>(p + off),
                                 static_cast<size_t>(len - off));
            }
            return;
        }
        ESP_LOGW(TAG, "binary frame without audio header (%d bytes)", len);
        return;
    }
    if (event == WEBSOCKET_EVENT_ERROR) {
        auto* ev = static_cast<esp_websocket_event_data_t*>(data);
        if (ev != nullptr) {
            ESP_LOGW(TAG, "websocket error: type=%d tls=0x%x stack=%d pending=%d",
                     (int)ev->error_handle.error_type,
                     (unsigned)ev->error_handle.esp_tls_last_esp_err,
                     ev->error_handle.esp_tls_stack_err, self->pending_.load());
        } else {
            ESP_LOGW(TAG, "websocket error");
        }
        self->connected_.store(false);
        return;
    }
    if (event == WEBSOCKET_EVENT_CLOSED) {
        auto* ev = static_cast<esp_websocket_event_data_t*>(data);
        ESP_LOGI(TAG, "websocket closed (code=%d pending=%d)",
                 ev ? ev->close_status_code : -1, self->pending_.load());
        self->connected_.store(false);
    }
}

bool EdgeSession::Connect(const std::string& voice, int speed) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // Reuse a live session across play/stop cycles: destroying and recreating
    // the websocket client each time fragments internal heap and eventually
    // makes xTaskCreate (Error create websocket task) fail.
    if (connected_ && client_ != nullptr) {
        voice_ = voice;
        speed_ = speed;
        ResetCounters();
        return true;
    }
    Disconnect();
    voice_ = voice;
    speed_ = speed;

    std::string token = TtsMakeSecMsGec();  // from tts_client
    char url[640];
    snprintf(url, sizeof(url),
             "wss://speech.platform.bing.com/consumer/speech/synthesize/readaloud/edge/v1"
             "?TrustedClientToken=6A5AA1D4EAFF4E9FB37E23D68491D6F4"
             "&ConnectionId=%s&Sec-MS-GEC=%s&Sec-MS-GEC-Version=2-155.0.4283.45",
             MakeUuid4Hex().c_str(), token.c_str());

    esp_websocket_client_config_t cfg = {};
    cfg.uri = url;
    cfg.disable_auto_reconnect = true;
    cfg.buffer_size = 4096;
    cfg.network_timeout_ms = 15000;  // TLS/TCP connect must fail fast; retries are cheap
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.task_stack = 6144;
    cfg.user_agent =
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/143.0.0.0 Safari/537.36 Edg/143.0.0.0";

    client_ = esp_websocket_client_init(&cfg);
    if (!client_) return false;
    esp_websocket_client_append_header(client_, "Origin",
        "chrome-extension://jdiccldimpdaibmpdkjnbmckianbfold");
    esp_websocket_client_append_header(client_, "Pragma", "no-cache");
    esp_websocket_client_append_header(client_, "Cache-Control", "no-cache");
    esp_websocket_client_append_header(client_, "Accept-Encoding", "gzip, deflate, br, zstd");
    esp_websocket_client_append_header(client_, "Accept-Language", "en-US,en;q=0.9");
    std::string cookie = "muid=" + MakeUuid4Hex() + ";";
    esp_websocket_client_append_header(client_, "Cookie", cookie.c_str());
    esp_websocket_register_events(client_, WEBSOCKET_EVENT_ANY, EventHandler, this);
    if (esp_websocket_client_start(client_) != ESP_OK) {
        esp_websocket_client_destroy(client_);
        client_ = nullptr;
        return false;
    }
    // Wait for connected, then send speech.config once.
    int waited = 0;
    while (!connected_ && waited < 10000) {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
    }
    if (!connected_) {
        esp_websocket_client_close(client_, pdMS_TO_TICKS(1000));
        esp_websocket_client_destroy(client_);
        client_ = nullptr;
        return false;
    }
    std::string ts = JsDateString();
    std::string cfg_msg = "X-Timestamp:" + ts + "\r\n"
        "Content-Type:application/json; charset=utf-8\r\nPath:speech.config\r\n\r\n"
        "{\"context\":{\"synthesis\":{\"audio\":{\"metadataoptions\":"
        "{\"sentenceBoundaryEnabled\":\"false\",\"wordBoundaryEnabled\":\"false\"},"
        "\"outputFormat\":\"audio-24khz-48kbitrate-mono-mp3\"}}}}\r\n";
    esp_websocket_client_send_text(client_, cfg_msg.c_str(), cfg_msg.size(), pdMS_TO_TICKS(5000));
    // Do NOT clear the audio ring here: a reconnect must let already received
    // audio keep playing while the new session is set up.
    ResetCounters();
    ESP_LOGI(TAG, "connected, speech.config sent");
    return true;
}

void EdgeSession::Disconnect() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (client_ != nullptr) {
        esp_websocket_client_close(client_, pdMS_TO_TICKS(1000));
        esp_websocket_client_destroy(client_);
        client_ = nullptr;
    }
    connected_ = false;
    pending_ = 0;
}

bool EdgeSession::Speak(const std::string& text) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!connected_ || client_ == nullptr || text.empty()) return false;
    std::string esc;
    XmlEscape(text, esc);
    char rate[16];
    snprintf(rate, sizeof(rate), "%+d%%", speed_);
    std::string ssml =
        "<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' "
        "xml:lang='en-US'><voice name='" + FullVoiceName(voice_) +
        "'><prosody pitch='+0Hz' rate='" + rate + "' volume='+0%'>" + esc +
        "</prosody></voice></speak>";
    std::string msg = "X-RequestId:" + MakeUuid4Hex() + "\r\n"
        "Content-Type:application/ssml+xml\r\nX-Timestamp:" + JsDateString() +
        "Z\r\nPath:ssml\r\n\r\n" + ssml;
    pending_++;
    int n = esp_websocket_client_send_text(client_, msg.c_str(), msg.size(), pdMS_TO_TICKS(5000));
    if (n < 0) pending_--;
    return n > 0;
}

void EdgeSession::Reset() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    ring_.Reset();
    ResetCounters();
    id3_sent_ = false;
}

// Keep buffered audio; only forget the requests the old connection owed us.
void EdgeSession::ResetCounters() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    pending_ = 0;
    done_count_ = 0;
}

// Edge TTS emits MPEG2 frames; the simple decoder only detects MP3 when the
// stream starts with an ID3 tag, so prefix one empty ID3v2 tag per session.
void EdgeSession::WriteAudio(const uint8_t* data, size_t len) {
    static const uint8_t kId3[10] = {'I', 'D', '3', 3, 0, 0, 0, 0, 0, 0};
    if (!id3_sent_) {
        ring_.Write(kId3, sizeof(kId3));
        id3_sent_ = true;
    }
    // AudioRing::Write 在满 1 秒后只写部分字节；漏写会截断 MP3 帧，
    // 表现为爆音。写不满就重试，并记下最终丢了多少。
    size_t off = 0;
    while (off < len) {
        size_t n = ring_.Write(data + off, len - off);
        if (n == 0) {
            ESP_LOGW(TAG, "audio ring full, dropped %u bytes", (unsigned)(len - off));
            break;
        }
        off += n;
    }
}
