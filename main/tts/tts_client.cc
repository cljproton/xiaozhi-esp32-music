#include "tts_client.h"
#include "settings.h"

#include <esp_http_client.h>
#include <esp_websocket_client.h>
#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <esp_random.h>

// Edge TTS v2 Sec-MS-GEC token algorithm (verified against edge-tts).
extern "C" void edge_token_v2(uint64_t unix_time, char* out, size_t out_len);

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#define TAG "TtsClient"

namespace {

constexpr const char* kNs = "tts";
constexpr const char* kTrustedClientToken = "6A5AA1D4EAFF4E9FB37E23D68491D6F4";
constexpr const char* kSecMsGecVersion = "2-155.0.4283.45";
constexpr const char* kWssBase =
    "wss://speech.platform.bing.com/consumer/speech/synthesize/readaloud/edge/v1"
    "?TrustedClientToken=6A5AA1D4EAFF4E9FB37E23D68491D6F4";

const char* const kEdgeVoices[] = {
    // Chinese
    "zh-CN-XiaoxiaoNeural", "zh-CN-YunxiNeural", "zh-CN-XiaoyiNeural",
    "zh-CN-YunjianNeural", "zh-CN-YunyangNeural", "zh-CN-liaoning-XiaobeiNeural",
    "zh-CN-shaanxi-XiaoniNeural",
    // English
    "en-US-AriaNeural", "en-US-GuyNeural", "en-US-JennyNeural", "en-US-EmmaNeural",
    "en-GB-SoniaNeural", "en-GB-RyanNeural", "en-AU-NatashaNeural", "en-CA-ClaraNeural",
};

std::string HexUpper(const uint8_t* bytes, size_t len) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(hex[bytes[i] >> 4]);
        out.push_back(hex[bytes[i] & 0x0F]);
    }
    return out;
}

// 32-hex UUID v4: version nibble=4, variant bits=10 (matches uuid.uuid4().hex).
std::string MakeUuid4Hex() {
    uint8_t b[16];
    for (int i = 0; i < 16; ++i) b[i] = static_cast<uint8_t>(esp_random());
    b[6] = (b[6] & 0x0F) | 0x40;
    b[8] = (b[8] & 0x3F) | 0x80;
    static const char* H = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (int i = 0; i < 16; ++i) {
        out.push_back(H[b[i] >> 4]);
        out.push_back(H[b[i] & 0x0F]);
    }
    return out;
}

std::string XmlEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
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
    return out;
}

static std::string FullVoiceName(const std::string& voice) {
    size_t d1 = voice.find('-');
    if (d1 == std::string::npos) return voice;
    size_t d2 = voice.find('-', d1 + 1);
    if (d2 == std::string::npos) return voice;
    return "Microsoft Server Speech Text to Speech Voice (" + voice.substr(0, d2) + ", " +
           voice.substr(d2 + 1) + ")";
}

std::string MakeSsml(const std::string& text, const std::string& voice, int speed) {
    char rate[16];
    snprintf(rate, sizeof(rate), "%+d%%", speed);
    std::string escaped = XmlEscape(text);
    std::string buf =
        "<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' "
        "xml:lang='en-US'><voice name='" + FullVoiceName(voice) +
        "'><prosody pitch='+0Hz' rate='" + rate + "' volume='+0%'>" +
        escaped + "</prosody></voice></speak>";
    return buf;
}

struct EdgeCtx {
    const TtsRequest* request = nullptr;
    esp_websocket_client_handle_t client = nullptr;
    std::string text_chunks;
    bool connected = false;
    std::string buffer;
    bool done = false;
    bool ok = false;
    std::string error;
};

static std::string JsDateString() {
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

void EdgeEvent(void* arg, esp_event_base_t, int32_t event, void* data) {
    auto* ctx = static_cast<EdgeCtx*>(arg);
    if (event == WEBSOCKET_EVENT_CONNECTED) {
        ctx->client = static_cast<esp_websocket_client_handle_t>(data);
        ctx->connected = true;
    } else if (event == WEBSOCKET_EVENT_DATA) {
        auto* ev = static_cast<esp_websocket_event_data_t*>(data);
        if (ev->op_code == WS_TRANSPORT_OPCODES_BINARY && ev->data_len > 0) {
            ctx->buffer.append(ev->data_ptr, ev->data_len);
        }
    } else if (event == WEBSOCKET_EVENT_CLOSED) {
        ctx->done = true;
        ctx->ok = !ctx->buffer.empty();
    } else if (event == WEBSOCKET_EVENT_ERROR) {
        ctx->done = true;
        ctx->error = "websocket error";
    }
}

bool EdgeSynthesize(const TtsRequest& req) {
    char url[640];
    std::string conn_id = MakeUuid4Hex();
    std::string token = TtsMakeSecMsGec();
    snprintf(url, sizeof(url), "%s&ConnectionId=%s&Sec-MS-GEC=%s&Sec-MS-GEC-Version=%s",
             kWssBase, conn_id.c_str(), token.c_str(), kSecMsGecVersion);
    esp_websocket_client_config_t cfg = {};
    cfg.uri = url;
    cfg.disable_auto_reconnect = true;
    cfg.buffer_size = 4096;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.task_stack = 12288;
    cfg.user_agent =
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/143.0.0.0 Safari/537.36 Edg/143.0.0.0";
    EdgeCtx ctx;
    ctx.request = &req;
    cfg.user_context = &ctx;

    esp_websocket_client_handle_t client = esp_websocket_client_init(&cfg);
    if (!client) {
        req.on_done(false, "failed to init websocket");
        return false;
    }
    // Microsoft Edge TTS requires browser-like headers or returns HTTP 403.
    esp_websocket_client_append_header(client, "Origin", "chrome-extension://jdiccldimpdaibmpdkjnbmckianbfold");
    esp_websocket_client_append_header(client, "Pragma", "no-cache");
    esp_websocket_client_append_header(client, "Cache-Control", "no-cache");
    esp_websocket_client_append_header(client, "Accept-Encoding", "gzip, deflate, br, zstd");
    esp_websocket_client_append_header(client, "Accept-Language", "en-US,en;q=0.9");
    std::string muid_hex = MakeUuid4Hex();
    std::transform(muid_hex.begin(), muid_hex.end(), muid_hex.begin(), ::toupper);
    std::string cookie = "muid=" + muid_hex + ";";
    esp_websocket_client_append_header(client, "Cookie", cookie.c_str());
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, EdgeEvent, &ctx);
    if (esp_websocket_client_start(client) != ESP_OK) {
        esp_websocket_client_destroy(client);
        req.on_done(false, "failed to begin websocket");
        return false;
    }

    // Wait until connected, then send config + SSML from this task (not the
    // websocket event task, where sends can race with state changes).
    const int timeout_ms = 60000;
    int waited = 0;
    while (!ctx.connected && !ctx.done && waited < 10000) {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
    }
    if (ctx.connected && !ctx.done) {
        std::string ts = JsDateString();
        std::string cfg = "X-Timestamp:" + ts + "\r\n"
            "Content-Type:application/json; charset=utf-8\r\n"
            "Path:speech.config\r\n\r\n"
            "{\"context\":{\"synthesis\":{\"audio\":{\"metadataoptions\":"
            "{\"sentenceBoundaryEnabled\":\"true\",\"wordBoundaryEnabled\":\"false\"},"
            "\"outputFormat\":\"audio-24khz-48kbitrate-mono-mp3\"}}}}\r\n";
        esp_websocket_client_send_text(client, cfg.c_str(), cfg.size(), pdMS_TO_TICKS(5000));
        vTaskDelay(pdMS_TO_TICKS(100));
        std::string ssml = MakeSsml(req.text, req.voice, req.speed);
        std::string reqid = MakeUuid4Hex();
        std::string msg = "X-RequestId:" + reqid + "\r\n"
                          "Content-Type:application/ssml+xml\r\n"
                          "X-Timestamp:" + ts + "Z\r\n"
                          "Path:ssml\r\n\r\n" + ssml;
        esp_websocket_client_send_text(client, msg.c_str(), msg.size(), pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "Edge TTS SSML sent (%u bytes)", (unsigned)msg.size());
    }

    // Wait for audio. The server may not send a CLOSE event promptly, so once
    // audio starts arriving, finish after a short quiet period.
    waited = 0;
    size_t last_size = 0;
    int quiet = 0;
    while (waited < timeout_ms && !ctx.done) {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
        size_t now_size = ctx.buffer.size();
        if (now_size > last_size) {
            last_size = now_size;
            quiet = 0;
        } else if (now_size > 0) {
            quiet += 100;
            if (quiet >= 1500) break;  // audio finished
        }
    }
    esp_websocket_client_close(client, pdMS_TO_TICKS(1000));
    esp_websocket_client_destroy(client);
    // Server closes the connection right after streaming audio; that surfaces
    // as a transport ERROR event even though synthesis succeeded. Audio bytes
    // are the authoritative success signal.
    bool ok = !ctx.buffer.empty();
    ESP_LOGI(TAG, "Edge TTS result: %s (%u audio bytes)", ok ? "ok" : "failed",
             (unsigned)ctx.buffer.size());
    if (ok && !ctx.buffer.empty() && req.on_audio) {
        // Edge TTS starts with an MPEG2 0xFFF3 frame; GMF's MP3 type detector
        // only recognizes ID3 or MPEG1 sync, so prepend a minimal ID3v2 tag.
        static const uint8_t kId3Header[10] = {'I', 'D', '3', 3, 0, 0, 0, 0, 0, 0};
        req.on_audio(kId3Header, sizeof(kId3Header));
        req.on_audio(reinterpret_cast<const uint8_t*>(ctx.buffer.data()), ctx.buffer.size());
    }
    req.on_done(ok, ctx.error);
    return ok;
}

bool HttpSynthesize(const TtsRequest& req) {
    std::string url = TtsGetHttpUrl();
    if (url.empty()) {
        req.on_done(false, "HTTP TTS URL not configured");
        return false;
    }
    std::string body = std::string("{\"model\":\"tts-1\",\"input\":\"") + XmlEscape(req.text) +
                       "\",\"voice\":\"" + req.voice + "\",\"response_format\":\"mp3\"}";

    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    if (url.rfind("https://", 0) == 0) cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 30000;
    cfg.buffer_size = 2048;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        req.on_done(false, "failed to init http client");
        return false;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    std::string key = TtsGetApiKey();
    if (!key.empty()) {
        std::string auth = "Bearer " + key;
        esp_http_client_set_header(client, "Authorization", auth.c_str());
    }
    esp_http_client_set_post_field(client, body.c_str(), body.size());
    esp_err_t err = esp_http_client_open(client, body.size());
    bool ok = false;
    std::string error;
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status == 200) {
            char buf[1024];
            int read;
            while ((read = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
                if (req.on_audio) req.on_audio(reinterpret_cast<const uint8_t*>(buf), read);
            }
            ok = true;
        } else {
            error = "HTTP status " + std::to_string(status);
        }
    } else {
        error = esp_err_to_name(err);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    req.on_done(ok, error);
    return ok;
}

}  // namespace

// Sec-MS-GEC v2 token: 5-minute-aligned timestamp + custom hash.
std::string TtsMakeSecMsGec() {
    char token[128];
    edge_token_v2(static_cast<uint64_t>(time(nullptr)), token, sizeof(token));
    return token;
}

bool TtsSynthesize(const TtsRequest& request) {
    if (TtsGetProvider() == "http") return HttpSynthesize(request);
    // Microsoft rolls out the v2 backend region by region; a silent close or
    // 403 can happen intermittently. Retry with exponential backoff.
    static const int backoffs[] = {0, 1500, 3000, 6000};
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (attempt > 0) vTaskDelay(pdMS_TO_TICKS(backoffs[attempt]));
        if (EdgeSynthesize(request)) return true;
        ESP_LOGW(TAG, "Edge TTS attempt %d failed, retrying", attempt + 1);
    }
    return false;
}

std::string TtsGetProvider() {
    Settings s(kNs, false);
    auto v = s.GetString("provider", "edge");
    return v.empty() ? "edge" : v;
}
void TtsSetProvider(const std::string& v) { Settings s(kNs, true); s.SetString("provider", v); }
std::string TtsGetVoice() {
    Settings s(kNs, false);
    auto v = s.GetString("voice", "zh-CN-XiaoxiaoNeural");
    return v.empty() ? "zh-CN-XiaoxiaoNeural" : v;
}
void TtsSetVoice(const std::string& v) { Settings s(kNs, true); s.SetString("voice", v); }
int TtsGetSpeed() { Settings s(kNs, false); return s.GetInt("speed", 0); }
void TtsSetSpeed(int v) { Settings s(kNs, true); s.SetInt("speed", v); }
std::string TtsGetHttpUrl() { Settings s(kNs, false); return s.GetString("url", ""); }
void TtsSetHttpUrl(const std::string& v) { Settings s(kNs, true); s.SetString("url", v); }
std::string TtsGetApiKey() { Settings s(kNs, false); return s.GetString("api_key", ""); }
void TtsSetApiKey(const std::string& v) { Settings s(kNs, true); s.SetString("api_key", v); }

const char* const* TtsEdgeVoices(size_t* count) {
    *count = sizeof(kEdgeVoices) / sizeof(kEdgeVoices[0]);
    return kEdgeVoices;
}
