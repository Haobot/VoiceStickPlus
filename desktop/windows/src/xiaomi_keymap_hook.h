#pragma once

#include "xiaomi_buttons.h"
#include "xiaomi_keymap_interceptor.h"
#include "xiaomi_usage_tap.h"
#include "xiaomi_usage_tap_manager.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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
// usage tap 融合（Doc/Plan/xiaomi-remote-usage-tap.md §3.2.1）：back/volume_up/
// volume_down 三键系统不可见，由 tap 探针（WUDFHost 内 DLL 经管道回传 HID 报文）
// 提供信号——直触发状态机消费（released 注入/长按连发）；LL keydown 同键到达
// 时 CancelHold 让现有管线接管（RC001 类系统可见固件防双触发）；BREAK 兜底超时
// 前先查 tap 佐证表（GATT 层真源归属优先于物理键盘保守补偿）。
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

    // 幂等：已运行时仅刷新 key_map 快照。enable_tap 控制 usage tap 探针链路
    //（管道服务 + HostPid 监视 + 注入触发）是否随钩子启停。
    void Start(std::map<std::string, std::string> key_map, bool enable_tap);
    // 热更 key_map（配置对话框保存路径，无需重启钩子）。
    void UpdateKeymap(std::map<std::string, std::string> key_map);
    void Stop();
    bool running() const { return hook_ != nullptr; }
    // tap 链路最新状态（设置页状态行；未启用/未运行返回 nullopt）。
    std::optional<XiaomiUsageTapManager::LinkState> tap_state() const {
        if (!tap_enabled_) return std::nullopt;
        return tap_state_;
    }
    // 网关按键沿（P1 隧道融合）：网关模式软件路由键的 gateway_key 事件（协调器
    // on_gateway_key 转发，主线程调用）。设备归属由固件保证（遥控器只连 StickS3），
    // 无需 BREAK 佐证——直接查映射注入，按下沿 down 序、松开沿 up 序（支持按住）。
    void OnGatewayKeyEdge(std::string_view button, bool pressed);

private:
    static LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM w_param,
                                                 LPARAM l_param);
    // 主线程 message-only 窗口：接收 BREAK 佐证消息与兜底定时器。
    static LRESULT CALLBACK DispatchWndProc(HWND hwnd, UINT msg, WPARAM w_param,
                                            LPARAM l_param);
    // tap 管理器回调桥（管道/监视线程调用，仅 PostMessage 转主线程）。
    static void TapEdgeTrampoline(void* ctx, int button_index, bool pressed);
    static void TapStateTrampoline(void* ctx,
                                   XiaomiUsageTapManager::LinkState state);
    void RawInputThreadMain();
    void InjectVks(const std::vector<UINT>& vks, bool down);
    void ApplyAction(const XiaomiKeymapHookAction& action, const char* tag,
                     std::string_view button);
    // 主线程：BREAK 佐证到达的归属判定注入（wParam=按钮下标，lParam=1 遥控器）。
    void OnBreakMessage(int button_index, bool from_remote);
    // 主线程：BREAK 丢失兜底（tap 佐证优先，无佐证按物理键盘保守补偿）。
    void OnPendingTimer();
    // 主线程：tap 沿消费——佐证表登记 + 三键直触发（pressed 登记/released 注入）。
    void OnTapEdge(int button_index, bool pressed);
    // 主线程：tap 状态变化（记录 + 日志）。
    void OnTapState(XiaomiUsageTapManager::LinkState state);
    // 主线程：直触发长按连发轮询（kRepeatTimerMs 驱动）。
    void OnRepeatTimer();
    // 直触发 hold 存在性 ↔ 长按定时器开关联动。
    void SyncRepeatTimer();
    // 直触发三键（XiaomiButtonIsTapDirect 全集，PollRepeat/HasHold 遍历口径）。
    static constexpr std::string_view kTapDirectButtons[] = {
        "back", "volume_up", "volume_down"};

    HHOOK hook_ = nullptr;
    std::thread raw_input_thread_;
    DWORD raw_input_thread_id_ = 0;
    HWND raw_input_hwnd_ = nullptr;
    // 主线程异步判定窗口（message-only；LL 回调与 WndProc 同线程）。
    HWND dispatch_hwnd_ = nullptr;
    bool pending_timer_on_ = false;
    bool repeat_timer_on_ = false;
    // 注入键的 dwExtraInfo 标记（"XSKM"）：仅作诊断与第三方钩子区分。
    static constexpr ULONG_PTR kInjectExtraInfo = 0x58534B4D;
    // raw 线程 → 主线程的 BREAK 佐证消息。
    static constexpr UINT kMsgBreakEvidence = WM_APP + 0x4D4B;
    // 管道线程 → 主线程的 tap 沿消息（wParam=按钮下标，lParam=1 按下）。
    static constexpr UINT kMsgTapEdge = WM_APP + 0x5441;
    // 监视线程 → 主线程的 tap 链路状态消息（wParam=LinkState）。
    static constexpr UINT kMsgTapState = WM_APP + 0x5442;
    static constexpr UINT_PTR kPendingTimerId = 0x5144;
    static constexpr UINT kPendingTimerMs = 25;
    static constexpr UINT_PTR kRepeatTimerId = 0x5245;
    static constexpr UINT kRepeatTimerMs = 40;  // 最小重复间隔 40ms 的驱动粒度
    // key_map 快照（原子交换，钩子路径无锁读）。
    std::atomic<std::shared_ptr<const std::map<std::string, std::string>>>
        key_map_{nullptr};
    XiaomiKeymapInterceptor interceptor_;
    // usage tap 链路（enable_tap 时随 Start/Stop 启停）。
    bool tap_enabled_ = false;
    XiaomiUsageTapManager tap_manager_;
    XiaomiTapDirectKeys tap_direct_keys_;
    XiaomiTapEvidenceTable tap_evidence_;
    XiaomiUsageTapManager::LinkState tap_state_ =
        XiaomiUsageTapManager::LinkState::kNoHost;
    static XiaomiKeymapHook* active_instance_;
};

} // namespace voicestick
