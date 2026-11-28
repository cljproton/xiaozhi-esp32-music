#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Streaming text reader for WebDAV-hosted TXT books. Fetches the file in
// 32KB HTTP Range chunks into a small buffer; never loads the whole book.
// Handles GBK->UTF-8 conversion and UTF-8 character boundaries at chunk edges.
class TextStream {
public:
    TextStream();
    ~TextStream();

    // Open a book at a saved position. `raw_start` is the raw file offset the
    // saved chunk began at, `text_skip` the byte offset inside that chunk's
    // decoded text (both from a previous session; 0 = beginning).
    bool Open(const std::string& url, const std::string& username,
              const std::string& password, size_t raw_start = 0,
              size_t text_skip = 0);
    void Close();

    // Next sentence (terminated by punctuation / newline), UTF-8.
    // Returns false at end of book.
    bool NextSentence(std::string& out);

    // Progress: raw file offset of the current chunk + the consumed offset
    // inside it. Together they resume exactly (one extra chunk fetch).
    size_t RawPosition() const { return chunk_raw_start_; }
    size_t TextPosition() const { return pos_; }
    size_t TotalBytes() const { return total_bytes_; }
    // 已读的原始字节数：把解码后的偏移按比例折回原始字节，GBK 才不会虚高。
    size_t ConsumedBytes() const {
        if (buf_.empty() || raw_.empty())
            return chunk_raw_start_;
        return chunk_raw_start_ + pos_ * raw_.size() / buf_.size();
    }
    // 0-100；首个分块拿到文件大小之前为 0。
    int Percent() const {
        if (total_bytes_ == 0)
            return 0;
        size_t done = ConsumedBytes();
        if (done > total_bytes_)
            done = total_bytes_;
        return static_cast<int>(static_cast<uint64_t>(done) * 100 / total_bytes_);
    }
    bool EndOfFile() const { return eof_; }
    bool HasError() const { return error_; }

private:
    bool Refill();

    std::string url_;
    std::string username_;
    std::string password_;
    std::string buf_;         // decoded UTF-8 chunk
    std::string raw_;         // reused raw fetch buffer (avoids 32KB realloc churn)
    size_t pos_ = 0;          // read position within buf_
    size_t file_offset_ = 0;  // next raw byte to fetch
    size_t chunk_raw_start_ = 0;
    size_t total_bytes_ = 0;
    size_t pending_skip_ = 0;  // text_skip applied to the first chunk
    int failures_ = 0;         // consecutive failed Range requests
    bool error_ = false;       // gave up after repeated HTTP failures
    bool eof_ = false;
    bool need_convert_ = false;  // source is GBK
    std::string carry_;          // partial multibyte sequence at chunk edge
};
