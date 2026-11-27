#include "ebook_reader.h"
#include "gbk_table.h"

#include <string>

// WebDAV hrefs are percent-encoded; decode once, plus a second pass for
// double-encoded names (e.g. %255B -> %5B) seen on some servers.
std::string EbookDecodeName(const std::string& in) {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            int hi = hex(in[i + 1]), lo = hex(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(in[i]);
    }
    if (out.find('%') != std::string::npos) return EbookDecodeName(out);
    return out;
}

// Check whether a byte buffer is valid UTF-8.
bool IsValidUtf8(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { ++i; continue; }
        int need = 0;
        uint32_t cp = 0;
        if ((c & 0xE0) == 0xC0) { need = 1; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { need = 2; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { need = 3; cp = c & 0x07; }
        else return false;
        if (i + need >= s.size()) return false;
        for (int k = 1; k <= need; ++k) {
            unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        i += need + 1;
    }
    return true;
}

static void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) out.push_back(static_cast<char>(cp));
    else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Convert full GBK (incl. rare/extended hanzi) to UTF-8 via lookup table.
// ASCII passes through; unmappable bytes become '?'.
std::string GbkToUtf8(const std::string& in) {
    std::string out;
    out.reserve(in.size() * 2);
    for (size_t i = 0; i < in.size();) {
        unsigned char c = static_cast<unsigned char>(in[i]);
        if (c < 0x80) { out.push_back(static_cast<char>(c)); ++i; continue; }
        if (c >= 0x81 && c <= 0xFE && i + 1 < in.size()) {
            unsigned char c2 = static_cast<unsigned char>(in[i + 1]);
            int ti = -1;
            if (c2 >= 0x40 && c2 <= 0x7E) ti = c2 - 0x40;
            else if (c2 >= 0x80 && c2 <= 0xFE) ti = c2 - 0x40 - 1;
            if (ti >= 0) {
                uint16_t uni = kGbkUnicode[(c - 0x81) * 190 + ti];
                if (uni != 0) { AppendUtf8(out, uni); i += 2; continue; }
            }
        }
        out.push_back('?');
        ++i;
    }
    return out;
}
