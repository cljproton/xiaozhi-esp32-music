#pragma once

#include "handler_registry.h"
#include <string>

class MusicApiHandler {
public:
    // 将所有 /api/music/* handler 注册到 registry
    static void RegisterHandlers(HandlerRegistry& registry);
};