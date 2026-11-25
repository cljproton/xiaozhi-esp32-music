#include "ebook_reader.h"
#include "gbk_table.h"

#include "board.h"

#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <miniz.h>

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

// Check whether a byte buffer is valid UTF-8.
static bool IsValidUtf8(const std::string& s) {
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
static std::string GbkToUtf8(const std::string& in) {
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

uint16_t Rd16(const std::string& s, size_t p) {
    return static_cast<uint8_t>(s[p]) | (static_cast<uint8_t>(s[p + 1]) << 8);
}
uint32_t Rd32(const std::string& s, size_t p) {
    return static_cast<uint32_t>(static_cast<uint8_t>(s[p])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[p + 1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[p + 2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[p + 3])) << 24);
}

// Find a file in a ZIP archive, inflate it, return contents.
bool ZipExtract(const std::string& zip, const char* suffix, std::string& out) {
    // Find End of Central Directory
    size_t eocd = std::string::npos;
    for (size_t p = zip.size() >= 22 ? zip.size() - 22 : 0; p > 0 && p <= zip.size() - 22; --p) {
        if (Rd32(zip, p) == 0x06054b50) { eocd = p; break; }
    }
    if (eocd == std::string::npos) return false;
    uint16_t count = Rd16(zip, eocd + 10);
    size_t offset = Rd32(zip, eocd + 16);
    for (int i = 0; i < count && offset + 46 <= zip.size(); ++i) {
        if (Rd32(zip, offset) != 0x02014b50) break;
        uint16_t method = Rd16(zip, offset + 10);
        uint32_t comp_size = Rd32(zip, offset + 20);
        uint32_t uncomp_size = Rd32(zip, offset + 24);
        uint16_t name_len = Rd16(zip, offset + 28);
        uint16_t extra_len = Rd16(zip, offset + 30);
        uint16_t comment_len = Rd16(zip, offset + 32);
        uint32_t local_off = Rd32(zip, offset + 42);
        std::string name = zip.substr(offset + 46, name_len);
        bool match = suffix[0] == '\0' ? true :
                     (name.size() >= strlen(suffix) &&
                      name.compare(name.size() - strlen(suffix), strlen(suffix), suffix) == 0);
        if (match && local_off + 30 <= zip.size()) {
            uint16_t lname_len = Rd16(zip, local_off + 26);
            uint16_t lextra_len = Rd16(zip, local_off + 28);
            size_t data_off = local_off + 30 + lname_len + lextra_len;
            if (data_off + comp_size > zip.size()) return false;
            if (method == 0) {
                out = zip.substr(data_off, comp_size);
            } else if (method == 8) {
                out.resize(uncomp_size);
                size_t written = tinfl_decompress_mem_to_mem(
                    &out[0], uncomp_size, zip.data() + data_off, comp_size,
                    TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
                if (written == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) return false;
                out.resize(written);
            } else {
                return false;
            }
            return true;
        }
        offset += 46 + name_len + extra_len + comment_len;
    }
    return false;
}

// Strip HTML tags and decode common XML entities.
std::string StripHtml(const std::string& html) {
    std::string out;
    bool in_tag = false;
    for (size_t i = 0; i < html.size(); ++i) {
        char c = html[i];
        if (c == '<') { in_tag = true; continue; }
        if (c == '>') { in_tag = false; continue; }
        if (in_tag) continue;
        if (c == '&') {
            size_t semi = html.find(';', i);
            if (semi != std::string::npos && semi - i <= 12) {
                std::string ent = html.substr(i + 1, semi - i - 1);
                if (ent == "amp") out.push_back('&');
                else if (ent == "lt") out.push_back('<');
                else if (ent == "gt") out.push_back('>');
                else if (ent == "quot") out.push_back('"');
                else if (ent == "apos") out.push_back('\'');
                else if (ent == "nbsp") out.push_back(' ');
                else if (!ent.empty() && ent[0] == '#') {
                    unsigned code = std::strtoul(ent.c_str() + (ent[1] == 'x' || ent[1] == 'X' ? 2 : 1),
                                                 nullptr, ent[1] == 'x' || ent[1] == 'X' ? 16 : 10);
                    // encode as UTF-8
                    if (code < 0x80) out.push_back(static_cast<char>(code));
                    else if (code < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    }
                } else { out.push_back('&'); }
                i = semi;
                continue;
            }
        }
        out.push_back(c);
    }
    // Collapse whitespace
    std::string clean;
    bool last_space = false;
    for (char c : out) {
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') {
            if (!last_space) clean.push_back(' ');
            last_space = true;
        } else {
            clean.push_back(c);
            last_space = false;
        }
    }
    return clean;
}

void StripXmlTagsInPlace(std::string& s) {
    size_t p = 0;
    while ((p = s.find('<', p)) != std::string::npos) {
        size_t e = s.find('>', p);
        if (e == std::string::npos) break;
        s.erase(p, e - p + 1);
    }
}

}  // namespace

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
        book.paragraphs = EbookMergeParagraphs(data);
        book.name = url.substr(url.find_last_of('/') + 1);
        return book;
    }
    if (lower.find(".epub") != std::string::npos) {
        // Extract container.xml -> OPF path -> spine's first XHTML
        std::string container;
        if (!ZipExtract(data, "container.xml", container)) {
            book.error = "Invalid EPUB: no container.xml";
            return book;
        }
        StripXmlTagsInPlace(container);
        // Find full-path="..." in the (now tag-stripped) attributes is not reliable;
        // parse the raw container.xml for full-path attribute.
        size_t fp = container.find("full-path");
        // Re-read raw for attribute
        std::string raw_container;
        ZipExtract(data, "container.xml", raw_container);
        fp = raw_container.find("full-path");
        if (fp == std::string::npos) {
            book.error = "Invalid EPUB: no full-path";
            return book;
        }
        size_t q1 = raw_container.find('"', fp);
        size_t q2 = raw_container.find('"', q1 + 1);
        if (q1 == std::string::npos || q2 == std::string::npos) {
            book.error = "Invalid EPUB: bad full-path";
            return book;
        }
        std::string opf_path = raw_container.substr(q1 + 1, q2 - q1 - 1);
        std::string opf;
        if (!ZipExtract(data, opf_path.c_str(), opf)) {
            book.error = "Invalid EPUB: no OPF";
            return book;
        }
        // Extract the first <item ... href="..." ...> with XHTML media type in manifest.
        size_t manifest = opf.find("<manifest");
        size_t item = opf.find("<item", manifest == std::string::npos ? 0 : manifest);
        std::string href;
        std::string spine_item;
        while (item != std::string::npos) {
            size_t end = opf.find('>', item);
            if (end == std::string::npos) break;
            std::string tag = opf.substr(item, end - item);
            if (tag.find("application/xhtml+xml") != std::string::npos ||
                tag.find("text/html") != std::string::npos) {
                size_t h1 = tag.find("href=\"");
                if (h1 != std::string::npos) {
                    size_t h2 = tag.find('"', h1 + 6);
                    if (h2 != std::string::npos) { href = tag.substr(h1 + 6, h2 - h1 - 6); break; }
                }
            }
            item = opf.find("<item", end);
        }
        // Resolve href relative to OPF directory
        std::string chapter_path = href;
        size_t slash = opf_path.find_last_of('/');
        if (slash != std::string::npos && href.find('/') == std::string::npos) {
            chapter_path = opf_path.substr(0, slash + 1) + href;
        }
        std::string xhtml;
        if (href.empty() || !ZipExtract(data, chapter_path.c_str(), xhtml)) {
            book.error = "Invalid EPUB: no chapter";
            return book;
        }
        std::string text = StripHtml(xhtml);
        book.paragraphs = EbookMergeParagraphs(text);
        book.name = url.substr(url.find_last_of('/') + 1);
        return book;
    }

    book.error = "Unsupported format (only TXT and EPUB)";
    return book;
}
