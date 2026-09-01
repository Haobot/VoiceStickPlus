#pragma once

#include "xiaomi_atvv_session.h" // ShouldSuppressF5 / kF5SuppressWindowMs

#include <atomic>
#include <cstdint>

#include <windows.h>

namespace voicestick {

// 全局低级键盘钩子（WH_KEYBOARD_LL）：小米遥控器 2 Pro 的语音键按下时，遥控器
// 固件除发 ATVV 开麦帧外还会向 OS 多发一个 F5 键（触发浏览器刷新等副作用），且
// 按住期间 F5 以 ~30ms 自动重复。吞判定（对齐 MiVibe atvv_live_bridge）：
// - 近窗命中：最近 kF5SuppressWindowMs 内有开麦迹象（sink 由 BLE 层在 0x08/0x04
//   与每个音频帧更新）；
// - 关联等待：F5 经 HID 栈可能先于 ATVV 帧（ATT 栈）到达，首次 keydown 等待至多
//   80ms 再判（仅该键一次延迟）；
// - 序列闩锁：吞掉 keydown 后整个自动重复序列与其 keyup 一并吞掉。
// 其余按键一律放行（判定 O(1)，不影响系统其他热键）。配置开关：xiaomi_suppress_f5。
// 进程内单例（与 SelectionHotwordManager 同款 active_instance_ 指针）；Start/Stop
// 幂等，sink 指针指向 Win32App 持有的原子量（最后开麦迹象时刻，steady_clock ms）。
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
    // 钩子回调均在安装线程跑，无需原子：本次 F5 按下序列已被吞（重复/keyup 联动）。
    bool f5_sequence_suppressed_ = false;
    static VoiceF5Suppressor* active_instance_;
};

} // namespace voicestick
