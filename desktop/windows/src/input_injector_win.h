#pragma once

#include "voice_stick_coordinator.h"

#include <Windows.h>

#include <string>

namespace voicestick {

class InputInjectorWin : public InputInjector {
public:
    void Paste(const std::string& text, bool press_enter) override;
    void SendEnter() override;
    void SendArrowDown() override;

private:
    static std::wstring Utf16FromUtf8(const std::string& text);
    static void SendKey(WORD virtual_key, bool key_down, DWORD flags = 0);
    static void SendCtrlV();
};

} // namespace voicestick
