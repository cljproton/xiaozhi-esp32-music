#include "ebook_reader.h"
#include "gbk_table.h"

#include "board.h"

#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_log.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#define TAG "EbookReader"

namespace {

std::string HttpGet(const std::string& url, const std::string& username,
                    const std::string& password) {
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    if (url.rfind("https://", 0) == 0) cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 30000;
    cfg.buffer_size = 8192;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return "";
    if (!username.empty()) {
        esp_http_client_set_username(client, username.c_str());
        esp_http_client_set_password(client, password.c_str());
        esp_http_client_set_authtype(client, HTTP_AUTH_TYPE_BASIC);
    }
    std::string body;
    esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status == 200) {
            char buf[1024];
            int read;
            while ((read = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
                body.append(buf, read);
            }
        } else {
            ESP_LOGE(TAG, "HTTP GET status %d", status);
        }
    } else {
        ESP_LOGE(TAG, "HTTP GET open failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    return body;
}

}  // namespace

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

std::vector<std::string> EbookMergeParagraphs(const std::string& text, size_t target_chars) {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < text.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        cur.push_back(static_cast<char>(c));
        // Break at Chinese sentence punctuation or newlines
        if (c == '\n' || c == 0xE3) {  // 0xE3 begins UTF-8 punctuation (。！？；)
            if (i + 2 < text.size()) {
                unsigned char c2 = static_cast<unsigned char>(text[i + 1]);
                unsigned char c3 = static_cast<unsigned char>(text[i + 2]);
                bool punct = (c2 == 0x80 && (c3 == 0x82 || c3 == 0x81)) ||  // 。、
                             (c2 == 0x81 && (c3 == 0x81 || c3 == 0x82)) ||  // ！、
                             (c2 == 0x81 && c3 == 0x89) ||                   // ？
                             (c2 == 0x81 && c3 == 0x90);                     // ；
                if (punct) { cur += text.substr(i + 1, 2); i += 2; }
            }
        }
        if (cur.size() >= target_chars) {
            out.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

EbookBook EbookLoad(const std::string& url, const std::string& username,
                    const std::string& password) {
    EbookBook book;
    std::string data = HttpGet(url, username, password);
    if (data.empty()) {
        book.error = "Failed to download book";
        ESP_LOGE(TAG, "Failed to download book: %s", url.c_str());
        return book;
    }
    ESP_LOGI(TAG, "Downloaded book %u bytes", (unsigned)data.size());
    if (!IsValidUtf8(data)) {
        data = GbkToUtf8(data);
        ESP_LOGI(TAG, "Converted GBK book to UTF-8 (%u bytes)", (unsigned)data.size());
    }
    std::string lower = url;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower.find(".txt") != std::string::npos) {
        book.paragraphs = EbookMergeParagraphs(data, 300);
        book.name = url.substr(url.find_last_of('/') + 1);
        return book;
    }
    book.error = "Unsupported format (only TXT supported)";
    return book;
}
