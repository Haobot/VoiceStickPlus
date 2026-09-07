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
// 归属佐证（keyup 后置决策版，2026-09-07）：LL 钩子吞掉的按键不进系统翻译流
//（MAKE/BREAK raw 均不投递），按键时刻在用户态拿不到设备证据——先验判定必有
// 物理键误映射率。决策整体后置：keydown 只吞（零副作用零等待）；keyup 放行让
// BREAK 沿投递（hDevice = 可靠设备证据，Raw Input 线程按 VID/PID 0x2717/0x32B8
// 识别）；归属判定后由主线程注入映射 down+up 对（遥控器）或补偿原键对（物理
// 键），BREAK 异常丢失由 WM_TIMER 兜底补偿。按住连删退化为单击多次（已知取舍）。
//
// 进程单例（对齐 VoiceF5Suppressor）：Start/Stop 幂等；LL 钩子与异步判定窗口
// 都在主线程（须有消息泵），interceptor_ 无并发；Raw Input 线程内部泵独立
// 窗口，仅 PostMessage。key_map 快照原子交换（热更无锁读）。
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
    // 主线程 message-only 窗口：接收 BREAK 佐证消息与兜底定时器。
    static LRESULT CALLBACK DispatchWndProc(HWND hwnd, UINT msg, WPARAM w_param,
                                            LPARAM l_param);
    void RawInputThreadMain();
    void InjectVks(const std::vector<UINT>& vks, bool down);
    void ApplyAction(const XiaomiKeymapHookAction& action, const char* tag,
                     std::string_view button);
    // 主线程：BREAK 佐证到达的归属判定注入（wParam=按钮下标，lParam=1 遥控器）。
    void OnBreakMessage(int button_index, bool from_remote);
    // 主线程：BREAK 丢失兜底（物理键盘保守补偿）。
    void OnPendingTimer();

    HHOOK hook_ = nullptr;
    std::thread raw_input_thread_;
    DWORD raw_input_thread_id_ = 0;
    HWND raw_input_hwnd_ = nullptr;
    // 主线程异步判定窗口（message-only；LL 回调与 WndProc 同线程）。
    HWND dispatch_hwnd_ = nullptr;
    bool pending_timer_on_ = false;
    // 注入键的 dwExtraInfo 标记（"XSKM"）：仅作诊断与第三方钩子区分。
    static constexpr ULONG_PTR kInjectExtraInfo = 0x58534B4D;
    // raw 线程 → 主线程的 BREAK 佐证消息。
    static constexpr UINT kMsgBreakEvidence = WM_APP + 0x4D4B;
    static constexpr UINT_PTR kPendingTimerId = 0x5144;
    static constexpr UINT kPendingTimerMs = 25;
    // key_map 快照（原子交换，钩子路径无锁读）。
    std::atomic<std::shared_ptr<const std::map<std::string, std::string>>>
        key_map_{nullptr};
    XiaomiKeymapInterceptor interceptor_;
    static XiaomiKeymapHook* active_instance_;
};

} // namespace voicestick
