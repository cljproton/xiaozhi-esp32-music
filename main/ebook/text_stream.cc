#include "text_stream.h"
#include "ebook_reader.h"
#include "board.h"

#include <mbedtls/base64.h>
#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define TAG "TextStream"

namespace {

constexpr size_t kChunkSize = 32 * 1024;
constexpr size_t kMaxSentence = 180;  // bytes (~60 Chinese chars): keeps a
                                      // sentence's MP3 well below the ring size

// Number of trailing bytes that form an incomplete multibyte character.
size_t IncompleteTail(const std::string& s, bool gbk) {
    if (s.empty()) return 0;
    if (gbk) {
        return static_cast<unsigned char>(s.back()) >= 0x80 ? 1 : 0;
    }
    // Walk back over continuation bytes to find the last character's lead
    // byte; counting all trailing >=0x80 bytes is wrong for CJK text because
    // consecutive characters contain many continuation bytes.
    size_t start = s.size();
    while (start > 0 && (static_cast<unsigned char>(s[start - 1]) & 0xC0) == 0x80) --start;
    if (start == 0) return 0;
    unsigned char lead = static_cast<unsigned char>(s[start - 1]);
    size_t need = 0;
    if ((lead & 0xE0) == 0xC0) need = 2;
    else if ((lead & 0xF0) == 0xE0) need = 3;
    else if ((lead & 0xF8) == 0xF0) need = 4;
    else return 0;
    size_t have = s.size() - start + 1;  // include the lead byte itself
    return have < need ? have : 0;
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
                      const std::string& password, size_t raw_start,
                      size_t text_skip) {
    Close();
    url_ = url;
    username_ = username;
    password_ = password;
    file_offset_ = raw_start;
    pos_ = 0;
    eof_ = false;
    need_convert_ = false;
    carry_.clear();
    buf_.clear();
    raw_.clear();
    raw_.reserve(kChunkSize);
    pending_skip_ = text_skip;
    failures_ = 0;
    error_ = false;
    return true;  // actual fetch happens on first Refill
}

void TextStream::Close() {
    url_.clear();
    buf_.clear();
    raw_.clear();
    carry_.clear();
    pos_ = 0;
    file_offset_ = 0;
    chunk_raw_start_ = 0;
    total_bytes_ = 0;
    pending_skip_ = 0;
    failures_ = 0;
    error_ = false;
    eof_ = true;
}

bool TextStream::Refill() {
    if (eof_ || url_.empty()) return false;
    // Shared Http layer keeps Range reads consistent with other network paths.
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
    if (!http) return false;
    http->SetTimeout(20000);
    if (!username_.empty()) {
        std::string cred = username_ + ":" + password_;
        size_t olen = 0;
        mbedtls_base64_encode(nullptr, 0, &olen,
                              reinterpret_cast<const unsigned char*>(cred.data()), cred.size());
        std::string b64(olen, '\0');
        mbedtls_base64_encode(reinterpret_cast<unsigned char*>(b64.data()), b64.size(), &olen,
                              reinterpret_cast<const unsigned char*>(cred.data()), cred.size());
        // mbedtls counts the terminating NUL; a NUL inside the header makes the
        // server reject the Range request with 400.
        b64.resize(strlen(b64.c_str()));
        http->SetHeader("Authorization", "Basic " + b64);
    }
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "bytes=%u-%u", (unsigned)file_offset_,
             (unsigned)(file_offset_ + kChunkSize - 1));
    http->SetHeader("Range", hdr);
    bool got = false;
    auto opened = http->Open("GET", url_);
    if (opened) {
        auto status = http->GetStatusCode();
        if (status && (*status == 206 || *status == 200)) {
            bool partial = *status == 206;  // 200 = server ignored Range, body starts at 0
            size_t clen = http->GetBodyLength();
            std::string range = http->GetResponseHeader("Content-Range");
            size_t slash = range.find('/');
            if (partial && slash != std::string::npos) {
                unsigned long long total = strtoull(range.c_str() + slash + 1, nullptr, 10);
                if (total > 0) total_bytes_ = static_cast<size_t>(total);
            } else if (clen > 0) {
                total_bytes_ = partial ? file_offset_ + clen : clen;
            }
            // ReadAll() builds a second 32KB string and can abort when PSRAM is
            // fragmented after long music/radio sessions. Reuse raw_ instead.
            raw_.clear();
            char tmp[512];
            // A conformant server may ignore Range and answer 200 with the
            // whole file. Discard bytes up to our logical chunk offset before
            // collecting this 32KB slice.
            size_t skip = partial ? 0 : file_offset_;
            while (skip > 0) {
                size_t want = std::min(sizeof(tmp), skip);
                auto dropped = http->Read(tmp, want);
                if (!dropped || *dropped == 0) break;
                skip -= static_cast<size_t>(*dropped);
            }
            // Never let a server that ignores Range blow up into a whole-book
            // read; kChunkSize bytes is all this buffer is sized for.
            while (raw_.size() < kChunkSize) {
                size_t want = std::min(sizeof(tmp), kChunkSize - raw_.size());
                auto got = http->Read(tmp, want);
                if (!got || *got == 0) break;
                raw_.append(tmp, static_cast<size_t>(*got));
            }
            std::string& raw = raw_;
            if (!raw.empty()) {
                // The fetch starts at the previous chunk's incomplete tail, so
                // the tail bytes arrive again here; nothing to prepend.
                carry_.clear();
                // Detect encoding on a prefix without a cut UTF-8 character;
                // otherwise a chunk that ends mid-character makes the whole
                // UTF-8 book look like GBK and TTS reads mojibake.
                std::string probe = raw;
                size_t utf8_tail = IncompleteTail(probe, false);
                if (utf8_tail > 0 && utf8_tail < probe.size()) probe.resize(probe.size() - utf8_tail);
                need_convert_ = !IsValidUtf8(probe);
                chunk_raw_start_ = file_offset_;
                size_t cut = IncompleteTail(raw, need_convert_);
                if (cut > 0 && cut < raw.size()) raw.resize(raw.size() - cut);
                buf_ = need_convert_ ? GbkToUtf8(raw) : raw;
                if (!need_convert_ && buf_.size() >= 3 &&
                    static_cast<unsigned char>(buf_[0]) == 0xEF &&
                    static_cast<unsigned char>(buf_[1]) == 0xBB &&
                    static_cast<unsigned char>(buf_[2]) == 0xBF) {
                    buf_.erase(0, 3);  // UTF-8 BOM is not spoken text
                }
                file_offset_ += raw.size();
                pos_ = 0;
                if (pending_skip_ > 0) {
                    pos_ = std::min(pending_skip_, buf_.size());
                    pending_skip_ = 0;
                }
                got = true;
            }
        } else {
            ESP_LOGE(TAG, "range status %d", status ? *status : -1);
        }
    } else {
        ESP_LOGE(TAG, "range open failed: %s", opened.error().ToString().c_str());
    }
    http->Close();
    if (!got) {
        // A failed Range request is usually transient (server hiccup). Keep
        // eof_ false so the caller retries instead of declaring the book done.
        if (++failures_ >= 10) {
            eof_ = true;
            error_ = true;
        }
        return false;
    }
    failures_ = 0;
    if (total_bytes_ && file_offset_ >= total_bytes_) eof_ = true;
    return true;
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
            // Plain-text books are full of blank lines; sending them to the TTS
            // server spends a pipeline slot on silence and delays real audio.
            if (out.find_first_not_of(" \t\r\n") == std::string::npos) continue;
            return true;
        }
        // No progress possible; refill.
        if (!Refill()) return false;
    }
}
