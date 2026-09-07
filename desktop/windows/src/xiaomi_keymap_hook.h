#pragma once

#include "xiaomi_buttons.h"
#include "xiaomi_keymap_interceptor.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include <windows.h>

namespace voicestick {

// 小米遥控器按键映射消费端（Doc/Plan/xiaomi-keymap-consumer.md）：拦截遥控器
// HID 按键的 Windows 原生翻译，替换为 key_map 配置的映射键。
//
// 归属佐证：LL 钩子拿不到按键来源设备，用 Raw Input（RIDEV_INPUTSINK）线程按
// hDevice 的 VID/PID（0x2717/0x32B8）识别遥控器，记录「按钮 → 最近佐证时刻」；
// 钩子候选键的首次 keydown 在等待窗内查佐证（未命中视为物理键盘同名键放行）。
// 这是 MiVibe「WUDF/Frida 直读信号」的零注入替代。
//
// 拦截与注入：佐证命中 → 吞原始键（返回 1）+ SendInput 注入映射 KeySpec
//（带 dwExtraInfo 标记；自带 LLKHF_INJECTED，自家两个 LL 钩子均放行注入键）。
// 按住闩锁/keyup 关联由 XiaomiKeymapInterceptor 承担。
//
// 进程单例（对齐 VoiceF5Suppressor）：Start/Stop 幂等；LL 钩子在调用线程
//（主线程，须有消息泵）安装，Raw Input 线程内部泵独立窗口。key_map 快照
// 原子交换（UpdateKeymap 热更无锁读）。
class XiaomiKeymapHook {
public:
    XiaomiKeymapHook() = default;
    ~XiaomiKeymapHook();
    XiaomiKeymapHook(const XiaomiKeymapHook&) = delete;
    XiaomiKeymapHook& operator=(const XiaomiKeymapHook&) = delete;

    // 幂等：已运行时仅刷新 key_map 快照。
    void Start(std::map<std::string, std::string> key_map);
    // 热更 key_map（配置对话框保存路径，无需重启钩子）。
    void UpdateKeymap(std::map<std::string, std::string> key_map);
    void Stop();
    bool running() const { return hook_ != nullptr; }

private:
    static LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM w_param,
                                                 LPARAM l_param);
    void RawInputThreadMain();
    // 遥控器按键佐证：Raw Input 线程在 VID/PID 命中设备上报时记录按钮时刻。
    void RecordSignal(std::string_view button);
    std::int64_t LoadSignalMs(std::string_view button);
    bool WaitForSignal(std::string_view button, std::int64_t now_ms,
                       std::int64_t window_ms);
    void InjectVks(const std::vector<UINT>& vks, bool down);

    HHOOK hook_ = nullptr;
    std::thread raw_input_thread_;
    DWORD raw_input_thread_id_ = 0;
    HWND raw_input_hwnd_ = nullptr;
    // 注入键的 dwExtraInfo 标记（"XSKM"）：仅作诊断与第三方钩子区分。
    static constexpr ULONG_PTR kInjectExtraInfo = 0x58534B4D;
    // 按钮佐证时刻表（steady_clock ms；0 = 从未）。索引同
    // kXiaomiMappableButtons。写 Raw Input 线程、读钩子线程，relaxed 即可
    //（仅做时间窗关联，无顺序依赖）。
    std::atomic<std::int64_t> signal_ms_[kXiaomiMappableButtons.size()] = {};
    // key_map 快照（原子交换，钩子路径无锁读）。
    std::atomic<std::shared_ptr<const std::map<std::string, std::string>>>
        key_map_{nullptr};
    XiaomiKeymapInterceptor interceptor_;
    static XiaomiKeymapHook* active_instance_;
};

} // namespace voicestick
