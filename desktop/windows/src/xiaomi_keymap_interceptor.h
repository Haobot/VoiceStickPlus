#pragma once

#include <windows.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "key_spec.h"

namespace voicestick {

// 小米遥控器 2 Pro 按键的 kbdhid 翻译特征识别：(VK, 扫描码) → 可映射按钮 ID
//（Doc/Plan/xiaomi-keymap-consumer.md §1 特征表）。仅判断「是哪个遥控器候选键」，
// 不判定按键来源归属——物理键盘同名键（Home/方向/`）靠 Raw Input 佐证区分。
// 翻译特征事实来源为 MiVibe 真机验证，本项目按事实重新实现。
std::optional<std::string_view> XiaomiButtonFromVkScan(UINT vk, UINT scan_code);

// 按钮的佐证等待窗（ms）：tv/home/menu/power 用 kCorrelateWindowMs，其余用
// kFastWindowMs。钩子层的关联等待与决策判定窗保持一致（见 XiaomiKeymapHook）。
std::int64_t XiaomiKeymapCorrelateWindowMs(std::string_view button);

// 注入 VK 序列：down 为修饰键序（Ctrl/Alt/Shift/Win）+ 主键，up 为反序
//（先松主键再松修饰键，保证不向系统泄漏按住的修饰键状态）。
std::vector<UINT> XiaomiKeymapInjectDownVks(const KeySpec& spec);
std::vector<UINT> XiaomiKeymapInjectUpVks(const KeySpec& spec);

// 按键映射拦截决策（LL 钩子事件 → 吞原始键 + 注入映射键序列）。
struct XiaomiKeymapDecision {
    bool swallow = false;      // 吞原始键（钩子返回 1，不透传给焦点应用）
    std::vector<UINT> inject;  // 注入的 VK 序列（方向随事件，keydown 注 down 序、
                               // keyup 注 up 反序）；空 = 不注入
};

// 消费端决策状态机（纯逻辑，时钟与佐证时刻由外部注入，可单测）。归属佐证模型：
// LL 钩子候选键的 keydown 在等待窗内查「该按钮最近的 Raw Input 佐证时刻」，
// 命中即判定来自遥控器——吞原始键并注入映射键；未命中视为物理键盘同名键放行。
// 按住闩锁：吞过 keydown 后，该按钮的自动重复免再佐证、keyup 关联吞（松开阶段
// 佐证窗可能已过期，镜像 VoiceF5Suppressor 闩锁模式）。
class XiaomiKeymapInterceptor {
public:
    XiaomiKeymapDecision OnHookEvent(std::string_view button, bool is_down,
                                     std::int64_t now_ms,
                                     std::int64_t signal_ms,
                                     const std::map<std::string, std::string>& key_map);

    // 卸载/重配时清按住闩锁，避免旧按住状态泄漏到新会话。
    void Reset();

    // tv/home/menu/power 佐证等待窗（ms）。这四键与物理键盘冲突面更大且 HID 佐证
    // 事件可能晚到（MiVibe 真机参数），用长窗；其余按钮用快窗。
    static constexpr std::int64_t kCorrelateWindowMs = 60;
    static constexpr std::int64_t kFastWindowMs = 15;

private:
    bool IsHeldSwallowed(std::string_view button) const;
    // 按住序列已被吞的按钮集（keyup 关联与自动重复免佐证）。
    std::vector<std::string> held_swallowed_;
};

} // namespace voicestick
