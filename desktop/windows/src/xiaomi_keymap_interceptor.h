#pragma once

#include <windows.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "key_spec.h"
#include "xiaomi_usage_tap.h"  // XiaomiTapDirectAction（注入对，直触发/网关键共用）

namespace voicestick {

// 小米遥控器 2 Pro 按键的 kbdhid 翻译特征识别：(VK, 扫描码) → 可映射按钮 ID
//（Doc/Plan/xiaomi-keymap-consumer.md §1 特征表）。仅判断「是哪个遥控器候选键」，
// 不判定按键来源归属——物理键盘同名键（Home/方向/`）靠 Raw Input 佐证区分。
// 翻译特征事实来源为 MiVibe 真机验证，本项目按事实重新实现。
std::optional<std::string_view> XiaomiButtonFromVkScan(UINT vk, UINT scan_code);

// 按钮在 key_map 中的映射规格；未配置/空串显式取消/非法串返回 nullopt（放行
// 语义）。keymap 拦截器与 usage tap 直触发共用（同一 key_map 消费口径）。
std::optional<KeySpec> XiaomiMappedSpec(
    std::string_view button,
    const std::map<std::string, std::string>& key_map);

// 小米遥控器 2 Pro 的 HID VID/PID（Doc/Ref/protocol.md ATVV 设备档案）。
constexpr uint32_t kXiaomiRemoteVendorId = 0x2717;
constexpr uint32_t kXiaomiRemoteProductId = 0x32B8;

// Raw Input 设备接口路径（RIDI_DEVICENAME）是否为小米遥控器。BTHLE 遥控器在
// Raw Input 中呈现为 RIM_TYPEKEYBOARD，RIDI_DEVICEINFO 只填 keyboard 联合体
// 成员（hid.dwVendorId 恒 0，无法据此识别，2026-09-07 真机排查定案），VID/PID
// 需从接口路径解析：BTHLE 名含 "_Dev_VID&012717_PID&32b8_"，USB HID 名含
// "VID_2717&PID_32B8"。大小写不敏感。
bool XiaomiRawInputNameIsRemote(const std::wstring& device_name);

// 注入 VK 序列：down 为修饰键序（Ctrl/Alt/Shift/Win）+ 主键，up 为反序
//（先松主键再松修饰键，保证不向系统泄漏按住的修饰键状态）。
std::vector<UINT> XiaomiKeymapInjectDownVks(const KeySpec& spec);
std::vector<UINT> XiaomiKeymapInjectUpVks(const KeySpec& spec);

// LL 钩子/异步判定的处理动作。swallow=true 时钩子返回 1 吞原始键；inject
// 非空则按 down 方向 SendInput 注入，inject_up 非空时紧随其后追加一次 up
// 序注入（归属判定完成后一次补齐 down+up 对）。注入事件带 kInjectExtraInfo
// 自标记，不会被自家钩子再吞。
struct XiaomiKeymapHookAction {
    bool swallow = false;
    std::vector<UINT> inject;
    std::vector<UINT> inject_up;
};

// 网关软件路由键的长按连发状态机（纯逻辑，时钟与 key_map 由外部注入）：
// 按下沿由调用方注入 down 序并登记 hold（真按住语义——组合键映射按住时
// 修饰键保持），到达延迟节拍后 PollRepeat 产出完整 down+up 对（音量键
// 直触发同款 400/120 手感，Doc/Plan/xiaomi-remote-stick-gateway.md P1），
// 松开沿调用方注入 up 序并清除。无映射按键不登记（放行语义）。
class XiaomiGatewayKeyRepeater {
public:
    static constexpr std::int64_t kRepeatDelayMs = 400;
    // 连发间隔默认 120ms（音量键直触发同款）；用户可经按键映射对话框滑块
    // 调整（钳位 [30, 300]，热更对下一次重复立即生效）。
    static constexpr std::int64_t kRepeatIntervalMs = 120;
    static constexpr std::int64_t kMinIntervalMs = 30;
    static constexpr std::int64_t kMaxIntervalMs = 300;

    void SetRepeatIntervalMs(std::int64_t ms) {
        repeat_interval_ms_ = ms < kMinIntervalMs   ? kMinIntervalMs
                              : ms > kMaxIntervalMs ? kMaxIntervalMs
                                                    : ms;
    }
    std::int64_t repeat_interval_ms() const { return repeat_interval_ms_; }

    // pressed：有有效映射才登记 hold（无映射/空串取消不登记，放行语义）；
    // 重复抖动幂等：已有同键 hold 不重置节拍。
    void OnPressed(std::string_view button, std::int64_t now_ms,
                   const std::map<std::string, std::string>& key_map);
    // released：清除该键 hold（无 hold 幂等）。
    void OnReleased(std::string_view button);
    // 长按轮询（WM_TIMER 40ms 粒度驱动）：到节拍返回映射的完整注入对；
    // 未到点/无 hold/映射已取消（key_map 热更）→ nullopt。
    std::optional<XiaomiTapDirectAction> PollRepeat(
        std::string_view button, std::int64_t now_ms,
        const std::map<std::string, std::string>& key_map);
    bool HasHold() const { return !holds_.empty(); }
    // 断连/钩子卸载清全部（防按键状态卡死）。
    void Reset();

private:
    struct Hold {
        std::string button;
        std::int64_t pressed_ms = 0;
        std::int64_t last_fire_ms = 0;
    };

    std::vector<Hold>::iterator FindHold(std::string_view button);

    std::vector<Hold> holds_;
    std::int64_t repeat_interval_ms_ = kRepeatIntervalMs;
};

// 消费端决策状态机（纯逻辑，时钟与佐证时刻由外部注入，可单测）。
//
// 归属佐证模型（keyup 后置决策版，2026-09-07 三次迭代定案）：LL 钩子是 RIT
// 同步调用的，被钩子吞掉的按键不进系统翻译流——本次的 MAKE/BREAK raw 均不
// 投递，按键时刻在用户态拿不到任何设备证据，先验判定（信用热快注入）必然
// 存在物理键误映射率（真机：物理 Home 被删字）。因此决策整体后置：
//   - keydown：吞（零副作用，钩子零等待），登记 pending，不注入；
//   - keyup：放行（孤立 up 无害），让 BREAK 沿随放行投递——BREAK raw 的
//     hDevice 是可靠的设备证据；
//   - BREAK 佐证到达（~3ms）：遥控器 → 注入映射 down+up 对；物理键盘 →
//     补偿注入原键 down+up 对（功能无损，反馈延迟到松手）；
//   - 兜底定时器：keyup 后 kBreakEvidenceWindowMs 内无 BREAK（异常丢失）
//     → 按物理键盘补偿。
// 按住连删退化为单击多次（每次 keyup 判定注入一对），属该模型的已知取舍。
class XiaomiKeymapInterceptor {
public:
    // LL keydown。有映射的候选键一律吞 + 登记 pending（vk/scan 存入供物理
    // 键盘补偿注入），不注入任何键。
    XiaomiKeymapHookAction OnKeyDown(
        std::string_view button, UINT vk, UINT scan_code, std::int64_t now_ms,
        const std::map<std::string, std::string>& key_map);
    // LL keyup。pending 中：放行（swallow=false，让 BREAK 沿投递）并标记
    // 待判定；无 pending：放行。
    XiaomiKeymapHookAction OnKeyUp(
        std::string_view button, std::int64_t now_ms,
        const std::map<std::string, std::string>& key_map);
    // BREAK 沿佐证（raw 线程转主线程调用）。命中「已松开的 pending」：
    // from_remote=true 注入映射 down+up 对；false 补偿原键 down+up 对。
    // 无匹配 pending 返回 nullopt（残留/按住中/已兜底）。
    std::optional<XiaomiKeymapHookAction> OnBreakEvidence(
        std::string_view button, std::int64_t now_ms, bool from_remote,
        const std::map<std::string, std::string>& key_map);
    // 兜底超时（主线程 WM_TIMER 调用）：keyup 已放行但 kBreakEvidenceWindowMs
    // 内无 BREAK 佐证 → 按物理键盘补偿原键 down+up 对。无过期 pending 返回
    // nullopt。
    std::optional<XiaomiKeymapHookAction> OnPendingTimeout(
        std::string_view button, std::int64_t now_ms);
    // 「已松开待判定」pending（按钮, 松开时刻）列表快照，供 hook 层 WM_TIMER
    // 逐个触发 OnPendingTimeout；按住中的 pending 不在内。
    std::vector<std::pair<std::string, std::int64_t>> PendingAwaitingBreak()
        const;
    // 是否存在 pending（hook 层决定 SetTimer/KillTimer）。
    bool HasPending() const { return !pendings_.empty(); }

    // 卸载/重配时清全部状态，避免旧待判定状态泄漏到新会话。
    void Reset();

    // BREAK 佐证等待窗（ms）：keyup 放行后等设备证据的兜底时限。正常 BREAK
    // 在放行后 ~3ms 到达，200ms 覆盖异常丢失场景（含消息队列拥堵余量）。
    static constexpr std::int64_t kBreakEvidenceWindowMs = 200;

private:
    struct Pending {
        std::string button;
        UINT vk = 0;
        UINT scan = 0;
        std::int64_t released_ms = 0;  // 0 = 尚未松开（按住中）
    };

    std::vector<Pending>::iterator FindPending(std::string_view button);
    void ConsumePending(std::vector<Pending>::iterator it);

    // 已吞待判定的按键（按钮 → 原始 vk/scan/松开时刻）。
    std::vector<Pending> pendings_;
};

} // namespace voicestick
