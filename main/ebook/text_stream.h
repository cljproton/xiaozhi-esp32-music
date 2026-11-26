#pragma once

#include <cstddef>
#include <string>

// Streaming text reader for WebDAV-hosted TXT books. Fetches the file in
// 32KB HTTP Range chunks into a small buffer; never loads the whole book.
// Handles GBK->UTF-8 conversion and UTF-8 character boundaries at chunk edges.
class TextStream {
public:
    TextStream();
    ~TextStream();

    // Open a book. `start_offset` is a UTF-8 text byte offset saved from a
    // previous session (0 = from beginning). Returns false on error.
    bool Open(const std::string& url, const std::string& username,
              const std::string& password, size_t start_offset = 0);
    void Close();

    // Get the next sentence (terminated by Chinese punctuation / newline),
    // converted to UTF-8. Returns false at end of book.
    bool NextSentence(std::string& out);

    // Current position in the converted UTF-8 text (for progress saving).
    size_t Position() const { return pos_; }
    size_t TotalBytes() const { return total_bytes_; }

private:
    bool Refill();  // fetch next Range chunk into buffer

    std::string url_;
    std::string username_;
    std::string password_;
    std::string buf_;       // current decoded UTF-8 chunk
    size_t pos_ = 0;        // read position within buf_
    size_t file_offset_ = 0;  // next byte range to fetch from server
    size_t total_bytes_ = 0;
    bool eof_ = false;
    bool need_convert_ = false;  // source is GBK
    std::string carry_;  // partial multibyte sequence at chunk boundary
};
