// Copyright (c) 2026 Voice Stick contributors. All rights reserved.
//
// 本机麦克风模式按住说话热键（Doc/Plan/local-mic-mode.md 迭代二）：LL 键盘
// 钩子观察目标单键（默认右 Ctrl）的按下/释放，转发协调器建/收 local-mic 会话。
//
// 只观察不拦截（键仍投递给前台应用）：按住说话键在绝大多数应用单按无副作用，
// 且不拦截可保证该键原有组合键（如右 Ctrl+C）行为不变。
// 注入的合成键（LLKHF_INJECTED，含本产品自身注入）一律忽略，防自触发。
// 进程单例（对齐 XiaomiKeymapHook）：Start/Stop 幂等；LL 回调与派发窗口都在
// 主线程（须有消息泵），无并发。

#pragma once

#include <windows.h>

#include <functional>

namespace voicestick {

class MicModeHotkey {
public:
    MicModeHotkey() = default;
    ~MicModeHotkey();

    MicModeHotkey(const MicModeHotkey&) = delete;
    MicModeHotkey& operator=(const MicModeHotkey&) = delete;

    // 幂等：已运行时返回 true 不重复安装。
    bool Start(UINT vk);
    void Stop();
    bool running() const { return hook_ != nullptr; }

    // 主线程（派发窗口 WndProc）触发。
    std::function<void()> on_pressed;
    std::function<void()> on_released;

private:
    static LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM w_param,
                                                 LPARAM l_param);
    static LRESULT CALLBACK DispatchWndProc(HWND hwnd, UINT msg, WPARAM w_param,
                                            LPARAM l_param);
    void OnKey(bool down);

    HHOOK hook_ = nullptr;
    HWND dispatch_hwnd_ = nullptr;
    UINT vk_ = 0;
    bool key_down_ = false;  // 自动重复去抖：按下沿到释放沿之间只触发一次
    // LL 回调（钩子线程=主线程）→ 派发窗口消息 → WndProc 主线程处理。
    static constexpr UINT kMsgKeyDown = WM_APP + 0x4D43;  // 'MC'
    static constexpr UINT kMsgKeyUp = WM_APP + 0x4D44;
    static MicModeHotkey* active_instance_;
};

}  // namespace voicestick
