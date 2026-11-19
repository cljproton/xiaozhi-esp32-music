#include "system_api_handler.h"
#include "handler_registry.h"

void SystemApiHandler::RegisterHandlers(HandlerRegistry& registry) {
    // 登录接口已统一到 AuthApiHandler（/api/auth）。
    (void)registry;
}
