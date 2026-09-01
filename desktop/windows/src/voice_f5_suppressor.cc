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
    if (active_instance_ == this) active_instance_ = nullptr;
}

LRESULT CALLBACK VoiceF5Suppressor::LowLevelKeyboardProc(int code, WPARAM w_param,
                                                         LPARAM l_param) {
    if (code == HC_ACTION && (w_param == WM_KEYDOWN || w_param == WM_SYSKEYDOWN) &&
        active_instance_ && active_instance_->mic_open_sink_) {
        const auto* info = reinterpret_cast<const KBDLLHOOKSTRUCT*>(l_param);
        if (info->vkCode == VK_F5) {
            const auto now = NowSteadyMs();
            const auto last =
                active_instance_->mic_open_sink_->load(std::memory_order_relaxed);
            const bool suppress = ShouldSuppressF5(now, last, true);
            LogApp(std::string("f5 keydown mic_open_age_ms=") +
                   (last > 0 ? std::to_string(now - last) : std::string("never")) +
                   (suppress ? " -> suppress" : " -> pass"));
            if (suppress) {
                return 1; // 吞掉：小米语音键附带的 F5
            }
        }
    }
    return CallNextHookEx(nullptr, code, w_param, l_param);
}

} // namespace voicestick
