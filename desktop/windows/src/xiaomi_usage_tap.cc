#include "xiaomi_usage_tap.h"

#include <algorithm>

#include "key_spec.h"
#include "xiaomi_keymap_interceptor.h"

namespace voicestick {
namespace {

// RC003 全键 usage 表（方案 §1：MiVibe FORWARD_USAGES 与本项目 Raw Input/
// 描述符枚举实测互证）。volume_mute 系统可见性未定，仅识别不直触发。
struct UsageTrait {
    uint16_t usage;
    std::string_view button;
};

constexpr UsageTrait kUsageTraits[] = {
    {0x0028, "ok"},        {0x0035, "tv"},          {0x004A, "home"},
    {0x004F, "right"},     {0x0050, "left"},        {0x0051, "down"},
    {0x0052, "up"},        {0x0065, "menu"},        {0x0066, "power"},
    {0x007F, "volume_mute"}, {0x0080, "volume_up"}, {0x0081, "volume_down"},
    {0x00F1, "back"},
};

} // namespace

std::optional<std::vector<uint16_t>> ParseTapReportUsages(const uint8_t* data,
                                                          size_t size) {
    if (data == nullptr || size != 9) return std::nullopt;
    if (data[0] != 0x01 || data[1] != 0x00 || data[2] != 0x00) {
        return std::nullopt;
    }
    std::vector<uint16_t> usages;
    usages.reserve(3);
    for (int slot = 0; slot < 3; ++slot) {
        const uint16_t usage =
            static_cast<uint16_t>(data[3 + slot * 2]) |
            (static_cast<uint16_t>(data[4 + slot * 2]) << 8);
        if (usage != 0) usages.push_back(usage);
    }
    std::sort(usages.begin(), usages.end());
    usages.erase(std::unique(usages.begin(), usages.end()), usages.end());
    return usages;
}

std::optional<std::string_view> XiaomiButtonFromUsage(uint16_t usage) {
    if (usage == 0) return std::nullopt;
    for (const auto& trait : kUsageTraits) {
        if (trait.usage == usage) return trait.button;
    }
    return std::nullopt;
}

bool XiaomiButtonIsTapDirect(std::string_view button) {
    return button == "back" || button == "volume_up" || button == "volume_down";
}

std::optional<XiaomiUsageTapSession::Edges> XiaomiUsageTapSession::OnReport(
    const uint8_t* data, size_t size) {
    auto usages = ParseTapReportUsages(data, size);
    if (!usages.has_value()) return std::nullopt;

    Edges edges;
    // diff：新集合相对旧集合的增删即按下/松开沿；usage → 按钮，未知 usage 进
    // 诊断列表（不进活跃集合，后续报文该 usage 消失也不产生 released）。
    for (uint16_t usage : *usages) {
        const bool was_active =
            std::find(active_.begin(), active_.end(), usage) != active_.end();
        const auto button = XiaomiButtonFromUsage(usage);
        if (button.has_value()) {
            if (!was_active) edges.pressed.push_back(*button);
        } else {
            edges.unknown_usages.push_back(usage);
        }
    }
    for (uint16_t usage : active_) {
        if (std::find(usages->begin(), usages->end(), usage) == usages->end()) {
            const auto button = XiaomiButtonFromUsage(usage);
            if (button.has_value()) edges.released.push_back(*button);
        }
    }
    active_ = std::move(*usages);
    return edges;
}

XiaomiUsageTapSession::Edges XiaomiUsageTapSession::OnDisconnect() {
    Edges edges;
    for (uint16_t usage : active_) {
        const auto button = XiaomiButtonFromUsage(usage);
        if (button.has_value()) edges.released.push_back(*button);
    }
    active_.clear();
    return edges;
}

XiaomiTapRepeatTiming XiaomiTapRepeatTimingFor(std::string_view button) {
    // 初值取 MiVibe 真机值（方案 §6.1）；设置页可调后再接配置覆盖。
    if (button == "back") return {280, 40};
    if (button == "volume_up" || button == "volume_down") return {400, 120};
    return {};
}

std::vector<XiaomiTapDirectKeys::Hold>::iterator
XiaomiTapDirectKeys::FindHold(std::string_view button) {
    return std::find_if(holds_.begin(), holds_.end(),
                        [button](const Hold& hold) {
                            return hold.button == button;
                        });
}

std::optional<XiaomiTapDirectAction> XiaomiTapDirectKeys::MakeAction(
    std::string_view button,
    const std::map<std::string, std::string>& key_map) const {
    const auto spec = XiaomiMappedSpec(button, key_map);
    if (!spec.has_value()) return std::nullopt;
    XiaomiTapDirectAction action;
    action.inject = XiaomiKeymapInjectDownVks(*spec);
    action.inject_up = XiaomiKeymapInjectUpVks(*spec);
    return action;
}

std::optional<XiaomiTapDirectAction> XiaomiTapDirectKeys::OnPressed(
    std::string_view button, std::int64_t now_ms,
    const std::map<std::string, std::string>& key_map) {
    if (!XiaomiButtonIsTapDirect(button)) return std::nullopt;
    if (InCancelGrace(button, now_ms)) return std::nullopt;  // 乱序防御
    if (FindHold(button) != holds_.end()) return std::nullopt;  // 报文抖动幂等
    if (!XiaomiMappedSpec(button, key_map).has_value()) return std::nullopt;
    Hold hold;
    hold.button = std::string(button);
    hold.pressed_ms = now_ms;
    holds_.push_back(std::move(hold));
    return std::nullopt;
}

std::optional<XiaomiTapDirectAction> XiaomiTapDirectKeys::OnReleased(
    std::string_view button, std::int64_t /*now_ms*/,
    const std::map<std::string, std::string>& key_map) {
    const auto hold = FindHold(button);
    if (hold == holds_.end()) return std::nullopt;
    // 长按重复已发过：松手不补发；单击（未发过）：注入一次映射对。
    std::optional<XiaomiTapDirectAction> action;
    if (hold->last_fire_ms == 0) {
        action = MakeAction(button, key_map);
    }
    holds_.erase(hold);
    return action;
}

std::optional<XiaomiTapDirectAction> XiaomiTapDirectKeys::PollRepeat(
    std::string_view button, std::int64_t now_ms,
    const std::map<std::string, std::string>& key_map) {
    const auto hold = FindHold(button);
    if (hold == holds_.end()) return std::nullopt;
    const auto timing = XiaomiTapRepeatTimingFor(button);
    if (timing.delay_ms <= 0 || timing.interval_ms <= 0) return std::nullopt;
    if (hold->last_fire_ms == 0) {
        if (now_ms - hold->pressed_ms < timing.delay_ms) return std::nullopt;
    } else if (now_ms - hold->last_fire_ms < timing.interval_ms) {
        return std::nullopt;
    }
    auto action = MakeAction(button, key_map);
    if (action.has_value()) hold->last_fire_ms = now_ms;
    return action;
}

void XiaomiTapDirectKeys::CancelHold(std::string_view button,
                                     std::int64_t now_ms) {
    const auto hold = FindHold(button);
    if (hold != holds_.end()) holds_.erase(hold);
    cancelled_at_[std::string(button)] = now_ms;
}

bool XiaomiTapDirectKeys::InCancelGrace(std::string_view button,
                                         std::int64_t now_ms) const {
    const auto it = cancelled_at_.find(button);
    if (it == cancelled_at_.end()) return false;
    return now_ms >= it->second &&
           now_ms - it->second <= kCancelGraceMs;
}

bool XiaomiTapDirectKeys::HasHold(std::string_view button) const {
    return const_cast<XiaomiTapDirectKeys*>(this)->FindHold(button) !=
           holds_.end();
}

void XiaomiTapDirectKeys::Reset() {
    holds_.clear();
    cancelled_at_.clear();
}

void XiaomiTapEvidenceTable::OnEdge(std::string_view button,
                                    std::int64_t now_ms) {
    last_edge_[std::string(button)] = now_ms;
}

bool XiaomiTapEvidenceTable::HasRecentEdge(std::string_view button,
                                           std::int64_t now_ms,
                                           std::int64_t window_ms) const {
    const auto it = last_edge_.find(button);
    if (it == last_edge_.end()) return false;
    // 仅 [edge, edge+window] 命中：查询时刻早于沿时刻（时钟乱序/并发读写错位）
    // 不算佐证。
    return now_ms >= it->second && now_ms - it->second <= window_ms;
}

void XiaomiTapEvidenceTable::Reset() { last_edge_.clear(); }

} // namespace voicestick
