#pragma once

#include "handler_registry.h"
#include <cJSON.h>
#include <string>

namespace ebook_api {
void RegisterHandlers(HandlerRegistry& registry);

// List TXT/EPUB books in a WebDAV directory. Returns a cJSON array of
// {"url","name"} objects (caller owns the returned pointer), or nullptr on error
// with `error` filled.
cJSON* ListSourceBooks(const std::string& dir_url, const std::string& username,
                       const std::string& password, std::string& error);
}  // namespace ebook_api
