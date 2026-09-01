#pragma once

#include "xiaomi_atvv_session.h" // ShouldSuppressF5 / kF5SuppressWindowMs

#include <atomic>
#include <cstdint>

#include <windows.h>

namespace voicestick {

// 全局低级键盘钩子（WH_KEYBOARD_LL）：小米遥控器 2 Pro 的语音键按下时，遥控器
// 固件除发 ATVV MIC_OPEN 外还会向 OS 多发一个 F5 键（触发浏览器刷新等副作用）。
// 仅在「最近 kF5SuppressWindowMs 内有 MIC_OPEN」时吞掉 F5 按下，其余按键一律
// 放行（判定 O(1)，不影响系统其他热键）。配置开关：xiaomi_suppress_f5。
// 进程内单例（与 SelectionHotwordManager 同款 active_instance_ 指针）；Start/Stop
// 幂等，sink 指针指向 Win32App 持有的原子量（最后 MIC_OPEN 时刻，steady_clock ms）。
class VoiceF5Suppressor {
public:
    VoiceF5Suppressor() = default;
    ~VoiceF5Suppressor();
    VoiceF5Suppressor(const VoiceF5Suppressor&) = delete;
    VoiceF5Suppressor& operator=(const VoiceF5Suppressor&) = delete;

    // 幂等：已运行时仅更新 sink 指针。
    void Start(const std::atomic<std::int64_t>* mic_open_sink);
    void Stop();
    bool running() const { return hook_ != nullptr; }

private:
    static LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM w_param, LPARAM l_param);

    HHOOK hook_ = nullptr;
    const std::atomic<std::int64_t>* mic_open_sink_ = nullptr;
    static VoiceF5Suppressor* active_instance_;
};

} // namespace voicestick
