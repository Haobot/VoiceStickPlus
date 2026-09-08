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
    // back：仅 RC001 固件特征（消费页 AC Back → VK_BROWSER_BACK，MiVibe 记录）。
    // RC003 的 back usage 0xF1（键盘页非标准）被微软 HidOverGatt WUDF 宿主在
    // 翻译层内部丢弃（2026-09-07 三轮探针 + MiVibe 研读定案：固件有上报 9 字节
    // GATT 报文，系统输入链路全静默），VK_BACK/0x0E 事件全部为物理键盘污染，勿收录。
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

// 接口路径 vid/pid 标记解析（ASCII，小写化后处理；VID/PID 值均为十六进制）。
bool IsLowerWordChar(wchar_t c) {
    return (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9');
}
bool IsLowerHexDigit(wchar_t c) {
    return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
}
uint32_t LowerHexValue(wchar_t c) {
    return c <= L'9' ? static_cast<uint32_t>(c - L'0')
                     : static_cast<uint32_t>(c - L'a' + 10);
}

// 取路径中 vid/pid 标记（形如 "vid&012717" / "vid_2717"）的数值。标记前一个
// 字符必须是字母数字以外的分隔符（防 "devid" 内嵌误命中），标记后须随 '&' 或
// '_' 再接 1~8 位十六进制；找不到合法标记返回 nullopt。
std::optional<uint32_t> DeviceIdTokenValue(const std::wstring& device_name,
                                           const wchar_t* token) {
    std::wstring lower;
    lower.reserve(device_name.size());
    for (wchar_t c : device_name) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        lower.push_back(c);
    }
    const std::wstring token_str(token);
    size_t pos = 0;
    while ((pos = lower.find(token_str, pos)) != std::wstring::npos) {
        const size_t value_pos = pos + token_str.size();
        const bool boundary_before =
            pos == 0 || !IsLowerWordChar(lower[pos - 1]);
        const bool boundary_after =
            value_pos < lower.size() &&
            (lower[value_pos] == L'&' || lower[value_pos] == L'_');
        if (boundary_before && boundary_after) {
            uint32_t value = 0;
            size_t i = value_pos + 1;
            for (; i < lower.size() && IsLowerHexDigit(lower[i]); ++i) {
                value = value * 16 + LowerHexValue(lower[i]);
                if (i - value_pos > 8) break;  // 超长非标记值，视为不命中
            }
            if (i > value_pos + 1 && i - value_pos <= 9) return value;
        }
        ++pos;
    }
    return std::nullopt;
}

// 按钮在 key_map 中的映射规格；未配置/空串取消/非法串返回 nullopt（放行语义）。
std::optional<KeySpec> MappedSpec(
    std::string_view button,
    const std::map<std::string, std::string>& key_map) {
    const auto it = key_map.find(std::string(button));
    if (it == key_map.end()) return std::nullopt;
    return ParseKeySpec(it->second);
}

} // namespace

bool XiaomiRawInputNameIsRemote(const std::wstring& device_name) {
    const auto vid = DeviceIdTokenValue(device_name, L"vid");
    const auto pid = DeviceIdTokenValue(device_name, L"pid");
    if (!vid.has_value() || !pid.has_value()) return false;
    // BTHLE 容器名的 VID 字段实测为 6 位十六进制（RC-6459 为 "012717"，前两位
    // 非 VID 内容，疑似 Vendor ID Source 前缀；PID 为 4 位 "32b8"），USB HID 名
    // 均为 4 位。统一按低 16 位（VID/PID 本征宽度）比对，两种格式都命中。
    return (*vid & 0xFFFF) == kXiaomiRemoteVendorId &&
           (*pid & 0xFFFF) == kXiaomiRemoteProductId;
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

std::vector<XiaomiKeymapInterceptor::Pending>::iterator
XiaomiKeymapInterceptor::FindPending(std::string_view button) {
    return std::find_if(pendings_.begin(), pendings_.end(),
                        [button](const Pending& pending) {
                            return pending.button == button;
                        });
}

void XiaomiKeymapInterceptor::ConsumePending(
    std::vector<Pending>::iterator it) {
    pendings_.erase(it);
}

XiaomiKeymapHookAction XiaomiKeymapInterceptor::OnKeyDown(
    std::string_view button, UINT vk, UINT scan_code, std::int64_t /*now_ms*/,
    const std::map<std::string, std::string>& key_map) {
    XiaomiKeymapHookAction action;
    if (!MappedSpec(button, key_map).has_value()) return action;
    // 按住中的自动重复：持续吞（pending 未消费前不新建）。
    if (FindPending(button) != pendings_.end()) {
        action.swallow = true;
        return action;
    }
    Pending pending;
    pending.button = std::string(button);
    pending.vk = vk;
    pending.scan = scan_code;
    pendings_.push_back(std::move(pending));
    action.swallow = true;
    return action;
}

XiaomiKeymapHookAction XiaomiKeymapInterceptor::OnKeyUp(
    std::string_view button, std::int64_t now_ms,
    const std::map<std::string, std::string>& /*key_map*/) {
    XiaomiKeymapHookAction action;  // 默认放行
    const auto pending = FindPending(button);
    if (pending == pendings_.end()) return action;
    // 松开：放行让 BREAK 沿投递（hDevice 设备证据），标记待判定。孤立 up 对
    // 焦点应用无害；归属判定完成后由注入/补偿补齐完整 down+up 对。
    pending->released_ms = now_ms;
    return action;
}

std::optional<XiaomiKeymapHookAction> XiaomiKeymapInterceptor::OnBreakEvidence(
    std::string_view button, std::int64_t now_ms, bool from_remote,
    const std::map<std::string, std::string>& key_map) {
    const auto pending = FindPending(button);
    if (pending == pendings_.end()) return std::nullopt;
    if (pending->released_ms == 0) return std::nullopt;  // 按住中/未放行
    const UINT vk = pending->vk;
    ConsumePending(pending);
    XiaomiKeymapHookAction action;
    if (from_remote) {
        const auto spec = MappedSpec(button, key_map);
        if (!spec.has_value()) return std::nullopt;  // 判定前映射被取消：等效原生
        action.swallow = true;  // 动作语义：原事件已被吞，此为归属后的映射注入
        action.inject = XiaomiKeymapInjectDownVks(*spec);
        action.inject_up = XiaomiKeymapInjectUpVks(*spec);
        return action;
    }
    // 物理键盘同名键：补偿注入原键 down+up 对（功能无损，延迟到松手）。
    action.swallow = true;
    action.inject.push_back(vk);
    action.inject_up.push_back(vk);
    return action;
}

std::optional<XiaomiKeymapHookAction> XiaomiKeymapInterceptor::OnPendingTimeout(
    std::string_view button, std::int64_t now_ms) {
    const auto pending = FindPending(button);
    if (pending == pendings_.end()) return std::nullopt;
    if (pending->released_ms == 0 ||
        now_ms <= pending->released_ms + kBreakEvidenceWindowMs) {
        return std::nullopt;
    }
    // BREAK 异常丢失（正常 ~3ms 到达）：按物理键盘保守补偿。
    const UINT vk = pending->vk;
    ConsumePending(pending);
    XiaomiKeymapHookAction action;
    action.swallow = true;
    action.inject.push_back(vk);
    action.inject_up.push_back(vk);
    return action;
}

std::vector<std::pair<std::string, std::int64_t>>
XiaomiKeymapInterceptor::PendingAwaitingBreak() const {
    std::vector<std::pair<std::string, std::int64_t>> awaiting;
    for (const auto& pending : pendings_) {
        if (pending.released_ms != 0) {
            awaiting.emplace_back(pending.button, pending.released_ms);
        }
    }
    return awaiting;
}

void XiaomiKeymapInterceptor::Reset() { pendings_.clear(); }

} // namespace voicestick
