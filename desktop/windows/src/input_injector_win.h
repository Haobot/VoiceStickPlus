#pragma once

#include "key_spec.h"
#include "voice_stick_coordinator.h"

#include <Windows.h>

#include <string>

namespace voicestick {

class InputInjectorWin : public InputInjector {
public:
    void Paste(const std::string& text, bool press_enter) override;
    // 全选粘贴整框替换：快照剪贴板 → 写 text → Ctrl+A → Ctrl+V → 还原快照
    //（targeted 整框替换兜底；Ctrl+A 语义固定=全选，无光标算术无错位）。
    void ReplaceAllWithPaste(const std::string& text) override;
    void SendEnter() override;
    void SendArrowDown() override;
    void SendArrowUp() override;
    void SendKeyCombo(const KeySpec& spec) override;
    void MoveMouse(int dx, int dy) override;
    void ClickLeftButton() override;

private:
    static std::wstring Utf16FromUtf8(const std::string& text);
    static void SendKey(WORD virtual_key, bool key_down, DWORD flags = 0);
    static void SendCtrlV();
    static void SendCtrlA();
    // 写入 CF_UNICODETEXT 文本（OpenClipboard/Empty/SetClipboardData 全程确认
    // 返回值）；Paste 与 ReplaceAllWithPaste 共用。
    static bool WriteClipboard(const std::wstring& wide);
};

} // namespace voicestick
