#include "input_injector_win.h"

#include "clipboard_vault.h"
#include "log.h"

#include <optional>
#include <vector>

namespace voicestick {

void InputInjectorWin::Paste(const std::string& text, bool press_enter) {
    if (text.empty()) return;
    const std::wstring wide = Utf16FromUtf8(text);
    if (wide.empty()) return;

    // 注入借道剪贴板，粘贴后须完整还原用户原内容（文本以外的格式不做恢复会被
    // 永久覆盖，P1 clipboard_vault 真机验证语义）。快照拿不到就不恢复——不能
    // 因恢复失败误清用户剪贴板。
    std::optional<ClipboardSnapshot> clipboard_snapshot;
    try {
        clipboard_snapshot = ClipboardVault().Save();
    } catch (const std::runtime_error& e) {
        LogApp(std::string("clipboard snapshot failed, skip restore: ") + e.what());
    }

    if (OpenClipboard(nullptr)) {
        EmptyClipboard();
        const SIZE_T bytes = (wide.size() + 1) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (memory != nullptr) {
            void* target = GlobalLock(memory);
            if (target != nullptr) {
                memcpy(target, wide.c_str(), bytes);
                GlobalUnlock(memory);
                SetClipboardData(CF_UNICODETEXT, memory);
                memory = nullptr;
            }
            if (memory != nullptr) GlobalFree(memory);
        }
        CloseClipboard();
    }

    Sleep(40);
    SendCtrlV();
    if (press_enter) {
        Sleep(120);
        SendEnter();
    }

    if (clipboard_snapshot) {
        // 目标应用异步读剪贴板，立即恢复会粘贴出旧内容（P1 restore_delay=150ms）。
        Sleep(150);
        ClipboardVault().Restore(*clipboard_snapshot);
    }
}

std::wstring InputInjectorWin::Utf16FromUtf8(const std::string& text) {
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

void InputInjectorWin::SendKey(WORD virtual_key, bool key_down, DWORD flags) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = virtual_key;
    input.ki.dwFlags = flags | (key_down ? 0 : KEYEVENTF_KEYUP);
    SendInput(1, &input, sizeof(INPUT));
}

void InputInjectorWin::SendCtrlV() {
    SendKey(VK_CONTROL, true);
    SendKey('V', true);
    SendKey('V', false);
    SendKey(VK_CONTROL, false);
}

void InputInjectorWin::SendEnter() {
    SendKey(VK_RETURN, true);
    SendKey(VK_RETURN, false);
}

void InputInjectorWin::SendArrowDown() {
    SendKey(VK_DOWN, true);
    SendKey(VK_DOWN, false);
}

void InputInjectorWin::SendArrowUp() {
    SendKey(VK_UP, true);
    SendKey(VK_UP, false);
}

void InputInjectorWin::SendKeyCombo(const KeySpec& spec) {
    if (spec.vk == 0) return;
    // 修饰键按下。
    for (UINT mod : spec.modifiers) {
        SendKey(static_cast<WORD>(mod), true);
    }
    // 主键带 scan code：仅 wVk 时第三方输入法不响应（见 wechat_input_method_hotkey.cc）。
    const WORD scan = static_cast<WORD>(MapVirtualKeyW(spec.vk, MAPVK_VK_TO_VSC));
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = static_cast<WORD>(spec.vk);
    input.ki.wScan = scan;
    SendInput(1, &input, sizeof(INPUT));
    input.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(INPUT));
    // 修饰键逆序释放。
    for (auto it = spec.modifiers.rbegin(); it != spec.modifiers.rend(); ++it) {
        SendKey(static_cast<WORD>(*it), false);
    }
}

void InputInjectorWin::MoveMouse(int dx, int dy) {
    if (dx == 0 && dy == 0) return;
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;  // 相对移动
    SendInput(1, &input, sizeof(INPUT));
}

void InputInjectorWin::ClickLeftButton() {
    INPUT inputs[2]{};
    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, inputs, sizeof(INPUT));
}

} // namespace voicestick
