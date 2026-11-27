#pragma once

#include <string>

struct EbookBook {
    std::string name;
};

// Encoding helpers (shared with the streaming reader).
std::string EbookDecodeName(const std::string& in);
bool IsValidUtf8(const std::string& s);
std::string GbkToUtf8(const std::string& in);
