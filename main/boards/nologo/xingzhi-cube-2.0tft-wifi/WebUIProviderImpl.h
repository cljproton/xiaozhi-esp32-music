#pragma once

#include "boards/common/WebUIProviderImpl.h"

class XingzhiCubeWebUIProvider : public WebUIProviderImpl {
public:
    int GetScreenWidth() const override { return 240; }
    int GetScreenHeight() const override { return 320; }
    bool HasTouchscreen() const override { return true; }
    
    // 可选：自定义主题 CSS
    std::string GetThemeCSS() const override {
        return R"CSS(
/* Xingzhi Cube 2.0" TFT 专用样式 */
.controls button { border-radius: 12px; }
.field input, .field select { border-radius: 10px; }
)CSS";
    }
};