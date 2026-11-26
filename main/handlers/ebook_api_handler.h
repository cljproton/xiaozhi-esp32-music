#pragma once

#include "handler_registry.h"
#include <cJSON.h>
#include <string>

namespace ebook_api {
void RegisterHandlers(HandlerRegistry& registry);

// List TXT books in a WebDAV directory. offset/limit (0 = all) page the result;
// `*total` receives the number of matching books. Caller owns the returned array.
cJSON* ListSourceBooks(const std::string& dir_url, const std::string& username,
                       const std::string& password, int offset, int limit,
                       int& total, std::string& error);
}  // namespace ebook_api
