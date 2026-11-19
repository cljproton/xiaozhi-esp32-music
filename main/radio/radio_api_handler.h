#pragma once

#include <memory>
#include <string>

#include "handler_registry.h"

namespace radio {

class RadioSource;

// 电台管理接口注册到本项目现有的 HTTP 服务，不再单独监听 80 端口。
class RadioApiHandler {
public:
    static void RegisterHandlers(HandlerRegistry& registry, std::shared_ptr<RadioSource> source);
    // 返回 http://<设备IP>/radio ，用于 AI 指引用户打开网页
    static std::string GetWebPageUrl();
};

}  // namespace radio
