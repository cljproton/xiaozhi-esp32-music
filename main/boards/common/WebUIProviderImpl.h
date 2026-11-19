#pragma once

#include "web_ui/WebUIProvider.h"
#include "board.h"

class WebUIProviderImpl : public WebUIProvider {
public:
    int GetScreenWidth() const override { return 240; }
    int GetScreenHeight() const override { return 320; }
    bool HasTouchscreen() const override { return false; }
    AudioCodec* GetAudioCodec() const override { return Board::GetInstance().GetAudioCodec(); }
    Display* GetDisplay() const override { return Board::GetInstance().GetDisplay(); }
};