#include "text_stream.h"
#include "ebook_reader.h"

#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <cstdio>

#define TAG "TextStream"

namespace {

constexpr size_t kChunkSize = 32 * 1024;
constexpr size_t kMaxSentence = 600;  // bytes, ~200 Chinese chars

// Number of trailing bytes that form an incomplete multibyte character.
size_t IncompleteTail(const std::string& s, bool gbk) {
    if (s.empty()) return 0;
    size_t i = s.size();
    while (i > 0 && static_cast<unsigned char>(s[i - 1]) >= 0x80) --i;
    size_t tail = s.size() - i;
    if (tail == 0) return 0;
    if (gbk) return (tail % 2 == 1) ? 1 : 0;
    unsigned char lead = static_cast<unsigned char>(s[i]);
    size_t need = 0;
    if ((lead & 0xE0) == 0xC0) need = 2;
    else if ((lead & 0xF0) == 0xE0) need = 3;
    else if ((lead & 0xF8) == 0xF0) need = 4;
    else return 0;
    return (tail < need) ? tail : 0;
}

// Back up to the last complete UTF-8 character boundary.
size_t Utf8Boundary(const std::string& s, size_t limit) {
    if (limit >= s.size()) return s.size();
    size_t i = limit;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
    return i > 0 ? i : limit;
}

}  // namespace

TextStream::TextStream() = default;
TextStream::~TextStream() { Close(); }

bool TextStream::Open(const std::string& url, const std::string& username,
                      const std::string& password, size_t start_offset) {
    Close();
    url_ = url;
    username_ = username;
    password_ = password;
    file_offset_ = start_offset;
    pos_ = 0;
    eof_ = false;
    need_convert_ = false;
    carry_.clear();
    buf_.clear();
    return true;  // actual fetch happens on first Refill
}

void TextStream::Close() {
    url_.clear();
    buf_.clear();
    carry_.clear();
    pos_ = 0;
    file_offset_ = 0;
    total_bytes_ = 0;
    eof_ = true;
}

bool TextStream::Refill() {
    if (eof_ || url_.empty()) return false;
    esp_http_client_config_t cfg = {};
    cfg.url = url_.c_str();
    if (url_.rfind("https://", 0) == 0) cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 20000;
    cfg.buffer_size = 4096;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;
    if (!username_.empty()) {
        esp_http_client_set_username(client, username_.c_str());
        esp_http_client_set_password(client, password_.c_str());
        esp_http_client_set_authtype(client, HTTP_AUTH_TYPE_BASIC);
    }
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "bytes=%u-%u", (unsigned)file_offset_,
             (unsigned)(file_offset_ + kChunkSize - 1));
    esp_http_client_set_header(client, "Range", hdr);
    esp_err_t err = esp_http_client_open(client, 0);
    bool got = false;
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status == 206 || status == 200) {
            int64_t clen = esp_http_client_get_content_length(client);
            if (clen > 0) total_bytes_ = file_offset_ + static_cast<size_t>(clen);
            std::string raw;
            char tmp[512];
            int n;
            while ((n = esp_http_client_read(client, tmp, sizeof(tmp))) > 0) raw.append(tmp, n);
            if (!raw.empty()) {
                if (!carry_.empty()) { raw = carry_ + raw; carry_.clear(); }
                // Detect encoding on every chunk (a text file has one encoding;
                // checking each chunk also covers resume from a non-zero offset).
                need_convert_ = !IsValidUtf8(raw);
                size_t cut = IncompleteTail(raw, need_convert_);
                if (cut > 0 && cut < raw.size()) {
                    carry_ = raw.substr(raw.size() - cut);
                    raw.resize(raw.size() - cut);
                }
                buf_ = need_convert_ ? GbkToUtf8(raw) : raw;
                file_offset_ += raw.size();
                pos_ = 0;
                got = true;
            }
        } else {
            ESP_LOGE(TAG, "range status %d", status);
        }
    } else {
        ESP_LOGE(TAG, "range open failed: %s", esp_err_to_name(err));
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (!got || (total_bytes_ && file_offset_ >= total_bytes_)) eof_ = true;
    return got;
}

bool TextStream::NextSentence(std::string& out) {
    while (true) {
        if (pos_ >= buf_.size()) {
            if (!Refill()) return false;
            continue;
        }
        size_t start = pos_;
        size_t i = pos_;
        size_t limit = std::min(buf_.size(), start + kMaxSentence);
        size_t end = limit;
        bool found = false;
        while (i < limit) {
            unsigned char c = static_cast<unsigned char>(buf_[i]);
            if (c == '\n' || c == '\r') {
                while (i < buf_.size() && (buf_[i] == '\n' || buf_[i] == '\r')) ++i;
                end = i;
                found = true;
                break;
            }
            if (i + 2 < buf_.size()) {
                unsigned char c1 = static_cast<unsigned char>(buf_[i + 1]);
                unsigned char c2 = static_cast<unsigned char>(buf_[i + 2]);
                if ((c == 0xE3 && c1 == 0x80 && (c2 == 0x82 || c2 == 0x81)) ||
                    (c == 0xEF && c1 == 0xBC && (c2 == 0x81 || c2 == 0x9F || c2 == 0x9B)) ||
                    (c == 0xE2 && c1 == 0x80 && c2 == 0xA6)) {
                    i += 3;
                    end = i;
                    found = true;
                    break;
                }
            }
            ++i;
        }
        if (!found) end = Utf8Boundary(buf_, limit);  // force split at max length
        if (end > start) {
            out = buf_.substr(start, end - start);
            pos_ = end;
            return true;
        }
        // No progress possible; refill.
        if (!Refill()) return false;
    }
}
