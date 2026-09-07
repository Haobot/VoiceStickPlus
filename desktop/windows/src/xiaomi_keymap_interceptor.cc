#include "xiaomi_keymap_interceptor.h"

#include <algorithm>

namespace voicestick {
namespace {

// kbdhid 翻译特征表（Doc/Plan/xiaomi-keymap-consumer.md §1）。扫描码列 0 表示
// 不校验扫描码（该 VK 本身即遥控器特征）；非 0 时要求扫描码匹配（VK 与物理键盘
// 冲突，需二级特征，或 VK 不定需扫描码兜底）。
struct VkScanTrait {
    UINT vk;
    UINT scan;           // 0 = 任意扫描码
    std::string_view button;
};

constexpr VkScanTrait kTraits[] = {
    {VK_BROWSER_BACK, 0, "back"},
    {VK_BROWSER_HOME, 0, "home"},
    {VK_HOME, 0, "home"},
    {VK_RETURN, 0, "ok"},
    {VK_UP, 0, "up"},
    {VK_DOWN, 0, "down"},
    {VK_LEFT, 0, "left"},
    {VK_RIGHT, 0, "right"},
    {VK_APPS, 0, "menu"},
    // tv：遥控器 usage 0x35 翻译为 VK_OEM_3 + 扫描码 0x29，与键盘 Grave 同特征，
    // 归属交给佐证；其余扫描码的 VK_OEM_3 是真键盘 ` 键，不进候选。
    {VK_OEM_3, 0x29, "tv"},
    // power：kbdhid 可能翻译为 VK_SLEEP、未知键 VK 0xFF，或仅扫描码 0x5E 可辨。
    {VK_SLEEP, 0, "power"},
    {0xFF, 0, "power"},
    {0, 0x5E, "power"},
    {VK_VOLUME_UP, 0, "volume_up"},
    {VK_VOLUME_DOWN, 0, "volume_down"},
};

// 佐证等待窗较大的按钮：HID 佐证事件可能晚于 LL 钩子到达且与物理键盘冲突面大。
constexpr std::string_view kLongWindowButtons[] = {"tv", "home", "menu", "power"};

bool IsLongWindowButton(std::string_view button) {
    return std::find(std::begin(kLongWindowButtons), std::end(kLongWindowButtons),
                     button) != std::end(kLongWindowButtons);
}

} // namespace

std::int64_t XiaomiKeymapCorrelateWindowMs(std::string_view button) {
    return IsLongWindowButton(button) ? XiaomiKeymapInterceptor::kCorrelateWindowMs
                                      : XiaomiKeymapInterceptor::kFastWindowMs;
}

std::optional<std::string_view> XiaomiButtonFromVkScan(UINT vk, UINT scan_code) {
    // LL 钩子事件总有非零 VK；纯扫描码特征（VK=0 + 0x5E）不在此路径出现，排除
    // 以免误吞真实键盘消息（真键盘消息 VK 不为 0 且扫描码 0x5E 不对应任何标准键）。
    if (vk == 0) return std::nullopt;
    for (const auto& trait : kTraits) {
        if (trait.vk == 0) {
            // 扫描码兜底特征：VK 不定（任意非零 VK 均可），仅匹配扫描码。
            if (trait.scan != 0 && scan_code == trait.scan) return trait.button;
            continue;
        }
        if (trait.vk == vk && (trait.scan == 0 || scan_code == trait.scan)) {
            return trait.button;
        }
    }
    return std::nullopt;
}

std::vector<UINT> XiaomiKeymapInjectDownVks(const KeySpec& spec) {
    std::vector<UINT> vks = spec.modifiers;
    vks.push_back(spec.vk);
    return vks;
}

std::vector<UINT> XiaomiKeymapInjectUpVks(const KeySpec& spec) {
    std::vector<UINT> vks = XiaomiKeymapInjectDownVks(spec);
    std::reverse(vks.begin(), vks.end());
    return vks;
}

bool XiaomiKeymapInterceptor::IsHeldSwallowed(std::string_view button) const {
    return std::any_of(held_swallowed_.begin(), held_swallowed_.end(),
                       [button](const std::string& held) {
                           return held == button;
                       });
}

XiaomiKeymapDecision XiaomiKeymapInterceptor::OnHookEvent(
    std::string_view button, bool is_down, std::int64_t now_ms,
    std::int64_t signal_ms,
    const std::map<std::string, std::string>& key_map) {
    XiaomiKeymapDecision decision;
    const auto it = key_map.find(std::string(button));
    if (it == key_map.end()) return decision;  // 未配置映射：原生行为
    const auto spec = ParseKeySpec(it->second);
    if (!spec.has_value()) return decision;  // 空串显式取消 / 非法串（防御）

    if (!is_down) {
        if (!IsHeldSwallowed(button)) return decision;  // down 未被吞，keyup 原样放行
        held_swallowed_.erase(
            std::remove_if(held_swallowed_.begin(), held_swallowed_.end(),
                           [button](const std::string& held) {
                               return held == button;
                           }),
            held_swallowed_.end());
        decision.swallow = true;
        decision.inject = XiaomiKeymapInjectUpVks(*spec);
        return decision;
    }

    // keydown：按住闩锁中的自动重复免再佐证（吞过 down 到 keyup 之间持续吞，
    // 保证按住序列不泄漏原始键）。
    if (IsHeldSwallowed(button)) {
        decision.swallow = true;
        decision.inject = XiaomiKeymapInjectDownVks(*spec);
        return decision;
    }

    // 首次 keydown：佐证窗内判定归属。signal 在未来（时钟乱序）不算命中。
    const std::int64_t window =
        IsLongWindowButton(button) ? kCorrelateWindowMs : kFastWindowMs;
    const bool correlated =
        signal_ms >= 0 && signal_ms <= now_ms && now_ms - signal_ms <= window;
    if (!correlated) return decision;  // 物理键盘同名键：放行

    held_swallowed_.emplace_back(button);
    decision.swallow = true;
    decision.inject = XiaomiKeymapInjectDownVks(*spec);
    return decision;
}

void XiaomiKeymapInterceptor::Reset() { held_swallowed_.clear(); }

} // namespace voicestick
