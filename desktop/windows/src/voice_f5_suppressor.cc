#include "voice_f5_suppressor.h"

#include "log.h"

#include <chrono>

namespace voicestick {

VoiceF5Suppressor* VoiceF5Suppressor::active_instance_ = nullptr;

namespace {

std::int64_t NowSteadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

VoiceF5Suppressor::~VoiceF5Suppressor() { Stop(); }

void VoiceF5Suppressor::Start(const std::atomic<std::int64_t>* mic_open_sink) {
    mic_open_sink_ = mic_open_sink;
    f5_sequence_suppressed_ = false;
    if (hook_ || !mic_open_sink_) return;
    active_instance_ = this;
    // LL 钩子在安装进程上下文执行，hMod 传本进程模块句柄（对齐
    // SelectionHotwordManager 的 WH_MOUSE_LL 用法）。
    hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                              GetModuleHandleW(nullptr), 0);
    if (!hook_) {
        active_instance_ = nullptr;
        LogApp("VoiceF5Suppressor: SetWindowsHookEx WH_KEYBOARD_LL failed err=" +
               std::to_string(GetLastError()));
    } else {
        LogApp("VoiceF5Suppressor: WH_KEYBOARD_LL hook installed");
    }
}

void VoiceF5Suppressor::Stop() {
    if (hook_) {
        UnhookWindowsHookEx(hook_);
        hook_ = nullptr;
    }
    f5_sequence_suppressed_ = false;
    if (active_instance_ == this) active_instance_ = nullptr;
}

LRESULT CALLBACK VoiceF5Suppressor::LowLevelKeyboardProc(int code, WPARAM w_param,
                                                         LPARAM l_param) {
    auto* self = active_instance_;
    if (code != HC_ACTION || !self || !self->mic_open_sink_) {
        return CallNextHookEx(nullptr, code, w_param, l_param);
    }
    const auto* info = reinterpret_cast<const KBDLLHOOKSTRUCT*>(l_param);
    // 合成按键（远程桌面/宏工具注入的 F5）不干预。
    if (info->vkCode != VK_F5 || (info->flags & LLKHF_INJECTED) != 0) {
        return CallNextHookEx(nullptr, code, w_param, l_param);
    }
    const bool is_down = (w_param == WM_KEYDOWN || w_param == WM_SYSKEYDOWN);
    const bool is_up = (w_param == WM_KEYUP || w_param == WM_SYSKEYUP);
    if (!is_down && !is_up) {
        return CallNextHookEx(nullptr, code, w_param, l_param);
    }

    if (is_up) {
        // 键程关联（对齐 MiVibe atvv_live_bridge）：仅当本次 F5 按下序列被吞时
        // 才吞抬起——松开阶段音频流已停、80ms 窗可能已过期，没有关联 keyup 会漏。
        const bool matched = self->f5_sequence_suppressed_;
        self->f5_sequence_suppressed_ = false;
        return matched ? 1 : CallNextHookEx(nullptr, code, w_param, l_param);
    }

    // keydown：同一序列的自动重复直接吞（不再等待/记日志）。
    if (self->f5_sequence_suppressed_) return 1;

    // 近窗命中（2 Pro 按住期间音频帧持续刷新锚点）直接吞。
    const auto now = NowSteadyMs();
    const auto last = self->mic_open_sink_->load(std::memory_order_relaxed);
    if (ShouldSuppressF5(now, last, true)) {
        self->f5_sequence_suppressed_ = true;
        LogApp("f5 keydown mic_open_age_ms=" + std::to_string(now - last) +
               " -> suppress");
        return 1;
    }

    // 竞态收口（真机实测 F5 可先于 ATVV 帧 ~1ms 到达，对齐 MiVibe 的关联等待）：
    // F5 首次按下时等待开麦迹象至多 kF5SuppressWindowMs，命中即吞并闩锁整个序列；
    // 超时放行（普通 F5 仅此一次最多延迟 80ms）。
    const auto wait_start = NowSteadyMs();
    const auto deadline = wait_start + kF5SuppressWindowMs;
    while (NowSteadyMs() < deadline) {
        Sleep(2);
        const auto cur = self->mic_open_sink_->load(std::memory_order_relaxed);
        if (ShouldSuppressF5(NowSteadyMs(), cur, true)) {
            self->f5_sequence_suppressed_ = true;
            LogApp("f5 keydown -> suppress (waited " +
                   std::to_string(NowSteadyMs() - wait_start) + "ms)");
            return 1;
        }
    }
    LogApp(std::string("f5 keydown mic_open_age_ms=") +
           (last > 0 ? std::to_string(now - last) : std::string("never")) + " -> pass");
    return CallNextHookEx(nullptr, code, w_param, l_param);
}

} // namespace voicestick
