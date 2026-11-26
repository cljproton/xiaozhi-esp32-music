#pragma once

#include <string>
#include <vector>

struct EbookBook {
    std::string name;
    std::vector<std::string> paragraphs;  // ready-to-speak text, merged by paragraph
    std::string error;
};

// Download and parse an ebook from an HTTP(S) URL (WebDAV).
// Supports TXT (UTF-8) and EPUB (ZIP containing XHTML). Other formats return error.
EbookBook EbookLoad(const std::string& url, const std::string& username = "",
                    const std::string& password = "");

// Merge sentences into paragraphs of roughly `target_chars` characters.
std::vector<std::string> EbookMergeParagraphs(const std::string& text, size_t target_chars = 300);

// Encoding helpers (shared with the streaming reader).
bool IsValidUtf8(const std::string& s);
std::string GbkToUtf8(const std::string& in);
