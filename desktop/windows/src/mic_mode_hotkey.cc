// Copyright (c) 2026 Voice Stick contributors. All rights reserved.

#include "mic_mode_hotkey.h"

#include "log.h"

#include <cstdio>

namespace voicestick {

MicModeHotkey* MicModeHotkey::active_instance_ = nullptr;

MicModeHotkey::~MicModeHotkey() {
    Stop();
}

bool MicModeHotkey::Start(UINT vk) {
    if (vk == 0) return false;
    if (hook_) {
        // 已运行：只换目标键（沿用钩子与窗口，避免重复安装）。
        vk_ = vk;
        key_down_ = false;
        return true;
    }
    active_instance_ = this;
    vk_ = vk;
    key_down_ = false;
    // LL 钩子与派发窗口都在主线程（须有消息泵；对齐 XiaomiKeymapHook）。
    WNDCLASSW wc{};
    wc.lpfnWndProc = DispatchWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"VoiceStickMicModeHotkeyDispatch";
    RegisterClassW(&wc);
    dispatch_hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                                     HWND_MESSAGE, nullptr, wc.hInstance,
                                     nullptr);
    if (!dispatch_hwnd_) {
        LogApp("MicModeHotkey: dispatch window create failed err=" +
               std::to_string(GetLastError()));
        active_instance_ = nullptr;
        return false;
    }
    hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                              GetModuleHandleW(nullptr), 0);
    if (!hook_) {
        LogApp("MicModeHotkey: SetWindowsHookEx WH_KEYBOARD_LL failed err=" +
               std::to_string(GetLastError()));
        DestroyWindow(dispatch_hwnd_);
        dispatch_hwnd_ = nullptr;
        active_instance_ = nullptr;
        return false;
    }
    char vk_text[8];
    std::snprintf(vk_text, sizeof(vk_text), "%02X", vk);
    LogApp("MicModeHotkey: started vk=0x" + std::string(vk_text));
    return true;
}

void MicModeHotkey::Stop() {
    if (hook_) {
        UnhookWindowsHookEx(hook_);
        hook_ = nullptr;
    }
    if (dispatch_hwnd_) {
        DestroyWindow(dispatch_hwnd_);
        dispatch_hwnd_ = nullptr;
    }
    if (active_instance_ == this) active_instance_ = nullptr;
    key_down_ = false;
}

void MicModeHotkey::OnKey(bool down) {
    if (down == key_down_) return;  // 按下沿自动重复 / 释放重复去抖
    key_down_ = down;
    if (down) {
        if (on_pressed) on_pressed();
    } else {
        if (on_released) on_released();
    }
}

LRESULT CALLBACK MicModeHotkey::LowLevelKeyboardProc(int code, WPARAM w_param,
                                                     LPARAM l_param) {
    auto* self = active_instance_;
    if (code != HC_ACTION || !self || !self->hook_) {
        return CallNextHookEx(nullptr, code, w_param, l_param);
    }
    const auto* info = reinterpret_cast<const KBDLLHOOKSTRUCT*>(l_param);
    // 合成键一律忽略（含本产品 InputInjector 的注入，防自触发环）。
    if ((info->flags & LLKHF_INJECTED) != 0) {
        return CallNextHookEx(nullptr, code, w_param, l_param);
    }
    if (info->vkCode != self->vk_) {
        return CallNextHookEx(nullptr, code, w_param, l_param);
    }
    const bool is_down = (w_param == WM_KEYDOWN || w_param == WM_SYSKEYDOWN);
    const bool is_up = (w_param == WM_KEYUP || w_param == WM_SYSKEYUP);
    if (is_down || is_up) {
        // 只观察不拦截（return 0 放行）；处理经消息派发回主线程，钩子回调内
        // 不做任何重活（LL 钩子超时会被系统摘除）。
        PostMessageW(self->dispatch_hwnd_, is_down ? kMsgKeyDown : kMsgKeyUp,
                     0, 0);
    }
    return CallNextHookEx(nullptr, code, w_param, l_param);
}

LRESULT CALLBACK MicModeHotkey::DispatchWndProc(HWND hwnd, UINT msg,
                                                WPARAM w_param, LPARAM l_param) {
    if (msg == kMsgKeyDown || msg == kMsgKeyUp) {
        if (auto* self = active_instance_) {
            self->OnKey(msg == kMsgKeyDown);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, w_param, l_param);
}

}  // namespace voicestick
