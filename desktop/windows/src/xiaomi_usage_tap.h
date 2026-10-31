#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

namespace voicestick {

// 小米遥控器 usage tap 纯逻辑层（Doc/Plan/xiaomi-remote-usage-tap.md §3.2.1 +
// §6 修订）：消费 WUDFHost 注入探针回传的 9 字节 HID 报文，产出按键沿。
//
// 背景：RC003 的 back(0x00F1)/volume_up(0x0080)/volume_down(0x0081) 走厂商页
// 0xFF00 报告，被微软 HidOverGatt WUDF 宿主在翻译层内部丢弃，系统输入链路
//（LL 钩子/Raw Input/焦点应用）全静默；tap 在丢弃点之前截获，是这三键唯一的
// 用户态信号源。其余 10 键系统可见，走现有 XiaomiKeymapHook 管线，tap 沿仅作
// BREAK 佐证兜底。时钟与报文由外部注入，纯逻辑可单测。

// 9 字节 tap 报文解析：`01 00 00` 前缀（report ID 1）+ 3×LE16 usage（0=空槽）。
// 返回升序去重非零 usage；格式非法（长度≠9/前缀不符/data 空）返回 nullopt。
// 报文格式为 MiVibe-Remote 真机验证事实，本项目按事实重新实现（GPL 隔离）。
std::optional<std::vector<uint16_t>> ParseTapReportUsages(const uint8_t* data,
                                                          size_t size);

// usage → 按钮 ID（13 键表，与 kXiaomiMappableButtons 同 ID 体系，另含
// volume_mute——RC003 是否有独立静音键待真机确认，先识别不直触发）。
// 未知 usage 返回 nullopt。
std::optional<std::string_view> XiaomiButtonFromUsage(uint16_t usage);

// 按钮是否为「系统不可见、tap 直触发」键：RC003 三键（back/volume_up/
// volume_down，厂商页 0xFF00 报告不被 kbdhid 翻译）。其余键系统可见，tap 沿
// 不直触发（防与现有 LL+Raw Input 管线双触发）。
bool XiaomiButtonIsTapDirect(std::string_view button);

// tap 报文流 → 按键沿（会话状态 = 上次活跃 usage 集合）。
class XiaomiUsageTapSession {
public:
    struct Edges {
        // 新按下/新松开的按钮（按 usage 升序）；string_view 指向静态表，安全。
        std::vector<std::string_view> pressed;
        std::vector<std::string_view> released;
        // 本次报文中出现的未知 usage（诊断日志用；不进活跃集合，不产沿）。
        std::vector<uint16_t> unknown_usages;
    };

    // 9 字节报文 → 与上次活跃集合的 diff 沿并更新状态；非法报文返回
    // nullopt 且状态不变。
    std::optional<Edges> OnReport(const uint8_t* data, size_t size);
    // 断连/管道 EOF：活跃集合全部生成 released 沿并清空状态（下游全释放，
    // 防按键卡死）。
    Edges OnDisconnect();

    bool active_empty() const { return active_.empty(); }

private:
    std::vector<uint16_t> active_;  // 升序去重
};

// 三键直触发长按重复节拍（初值取 MiVibe 真机值，§6.1：back 280/40ms，
// volume 400/120ms；非直触发键返回 0/0）。
struct XiaomiTapRepeatTiming {
    std::int64_t delay_ms = 0;     // 首次重复延迟（自 pressed 起）
    std::int64_t interval_ms = 0;  // 后续重复间隔
};
XiaomiTapRepeatTiming XiaomiTapRepeatTimingFor(std::string_view button);

// 三键直触发动作（语义同 XiaomiKeymapHookAction 的注入部分：down 序 + 紧随
// 的 up 反序，一次补齐完整按键对；无吞键概念——系统层本就无事件）。
struct XiaomiTapDirectAction {
    std::vector<UINT> inject;
    std::vector<UINT> inject_up;
};

// 三键直触发状态机（时钟注入，纯逻辑可单测）。
//
// 单击语义对齐现有 10 键的 keyup 后置：pressed 只登记 hold 不注入，released
// 注入一次映射 down+up 对（反馈延迟到松手，全键体验一致）。长按：pressed+
// delay 后每 interval 注入一对（连发，PollRepeat 由 hook 层定时器驱动），
// released 不再补发（松手不多一键）。
//
// 防双触发（RC001 类固件这些键系统可见）：hook 层在 LL keydown 收到同按钮
// 候选时调 CancelHold 让现有管线接管——直触发的最早注入点（released 或
// pressed+delay≥280ms）远晚于系统翻译到达，取消窗口充分。
class XiaomiTapDirectKeys {
public:
    // pressed：有有效映射且为直触发键 → 登记 hold（返回 nullopt）；无映射/
    // 空串取消/非直触发键/重复 pressed（报文抖动，幂等）→ nullopt 且不登记。
    std::optional<XiaomiTapDirectAction> OnPressed(
        std::string_view button, std::int64_t now_ms,
        const std::map<std::string, std::string>& key_map);
    // released：hold 存在且重复未发过 → 注入一次映射对并清除；hold 存在但
    // 重复已发过（长按松手）→ 只清除；无 hold → nullopt（残留沿）。
    std::optional<XiaomiTapDirectAction> OnReleased(
        std::string_view button, std::int64_t now_ms,
        const std::map<std::string, std::string>& key_map);
    // 长按重复轮询（WM_TIMER 驱动）：到节拍 → 注入一对映射对并登记已发；
    // 未到点/无 hold/无映射 → nullopt。
    std::optional<XiaomiTapDirectAction> PollRepeat(
        std::string_view button, std::int64_t now_ms,
        const std::map<std::string, std::string>& key_map);
    // LL keydown 到达时取消 hold（该设备该键系统可见，现有管线接管）。
    // 同时记录取消时刻：时钟乱序（keydown 先于 tap pressed 到达）时，
    // OnPressed 在抑制窗内拒绝登记，避免现有管线注入后 released 再补一发。
    void CancelHold(std::string_view button, std::int64_t now_ms);
    // CancelHold 抑制窗：LL keydown 与同报文 tap pressed 的乱序间隔远小于
    // 100ms 量级，150ms 留余量；正常连按（RC001 可见场景）落在窗内也语义
    // 正确——该次由现有管线接管，直触发不参与。
    static constexpr std::int64_t kCancelGraceMs = 150;
    bool HasHold(std::string_view button) const;
    // 断连清全部（防按键状态卡死）。
    void Reset();

private:
    struct Hold {
        std::string button;
        std::int64_t pressed_ms = 0;
        std::int64_t last_fire_ms = 0;  // 0 = 长按重复未发过（单击）
    };

    std::vector<Hold>::iterator FindHold(std::string_view button);
    // 当前映射构造注入对；映射无效（无条目/空串/非法 spec）返回 nullopt。
    std::optional<XiaomiTapDirectAction> MakeAction(
        std::string_view button,
        const std::map<std::string, std::string>& key_map) const;
    // now 时刻是否处于该按钮的取消抑制窗内。
    bool InCancelGrace(std::string_view button, std::int64_t now_ms) const;

    std::vector<Hold> holds_;
    std::map<std::string, std::int64_t, std::less<>> cancelled_at_;
};

// 「按钮 → 最近 tap 沿时刻」佐证表：BREAK 沿异常丢失（kBreakEvidenceWindowMs
// 内无 Raw Input BREAK）兜底超时时反查——命中说明遥控器刚发过该键（tap 在
// WUDFHost 上游，沿早于系统翻译），按遥控器注入映射而非错误按物理键盘补偿。
// tap 佐证优先级高于 Raw Input（GATT 层真源归属，物理键盘不可能产生 tap 信号）。
class XiaomiTapEvidenceTable {
public:
    // 默认窗口：tap released 沿与 LL keyup 几乎同时，兜底查询发生在 released
    // 后 ~200ms（interceptor 兜底窗）+ 定时器粒度，留余量 250ms；防相邻两次
    // 快速连按误命中，单槽只存最新沿。
    static constexpr std::int64_t kDefaultWindowMs = 250;

    void OnEdge(std::string_view button, std::int64_t now_ms);
    bool HasRecentEdge(std::string_view button, std::int64_t now_ms,
                       std::int64_t window_ms = kDefaultWindowMs) const;
    void Reset();

private:
    std::map<std::string, std::int64_t, std::less<>> last_edge_;
};

} // namespace voicestick
