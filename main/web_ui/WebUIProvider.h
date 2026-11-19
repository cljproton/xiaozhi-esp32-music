#pragma once

#include <string>
#include <vector>

class AudioCodec;
class Display;

class WebUIProvider {
public:
    virtual ~WebUIProvider() = default;

    // 板级硬件适配
    virtual int GetScreenWidth() const = 0;
    virtual int GetScreenHeight() const = 0;
    virtual bool HasTouchscreen() const = 0;
    virtual AudioCodec* GetAudioCodec() const = 0;
    virtual Display* GetDisplay() const = 0;

    // UI 定制
    virtual std::string GetThemeCSS() const { return ""; }
    virtual std::vector<std::string> GetExtraJSModules() const { return {}; }
};