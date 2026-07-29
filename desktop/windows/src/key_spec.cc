#include "key_spec.h"

#include <algorithm>
#include <cctype>

namespace voicestick {
namespace {

std::string Trim(const std::string& s) {
    size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin]))) ++begin;
    size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(begin, end - begin);
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](char c) {
                       return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                   });
    return s;
}

// 主键名 → VK。未知名返回 nullopt。
std::optional<UINT> MainKeyVkey(const std::string& lower) {
    if (lower.size() == 1) {
        const char ch = static_cast<char>(std::toupper(static_cast<unsigned char>(lower[0])));
        if (ch >= 'A' && ch <= 'Z') return static_cast<UINT>(ch);
        if (ch >= '0' && ch <= '9') return static_cast<UINT>(ch);
        return std::nullopt;
    }
    if (lower == "space") return VK_SPACE;
    if (lower == "enter" || lower == "return") return VK_RETURN;
    if (lower == "esc" || lower == "escape") return VK_ESCAPE;
    if (lower == "tab") return VK_TAB;
    if (lower == "backspace") return VK_BACK;
    if (lower == "delete") return VK_DELETE;
    if (lower == "insert") return VK_INSERT;
    if (lower == "up") return VK_UP;
    if (lower == "down") return VK_DOWN;
    if (lower == "left") return VK_LEFT;
    if (lower == "right") return VK_RIGHT;
    if (lower == "pageup") return VK_PRIOR;
    if (lower == "pagedown") return VK_NEXT;
    if (lower == "home") return VK_HOME;
    if (lower == "end") return VK_END;
    if (lower == "volumeup") return VK_VOLUME_UP;
    if (lower == "volumedown") return VK_VOLUME_DOWN;
    if (lower == "volumemute") return VK_VOLUME_MUTE;
    if (lower[0] == 'f' && lower.size() >= 2) {
        int num = 0;
        for (size_t i = 1; i < lower.size(); ++i) {
            if (lower[i] < '0' || lower[i] > '9') return std::nullopt;
            num = num * 10 + (lower[i] - '0');
            if (num > 24) return std::nullopt;  // 防 int 溢出
        }
        if (num >= 1 && num <= 24) return VK_F1 + (num - 1);
    }
    return std::nullopt;
}

std::string VkeyDisplayName(UINT vk) {
    if (vk >= 'A' && vk <= 'Z') return std::string(1, static_cast<char>(vk));
    if (vk >= '0' && vk <= '9') return std::string(1, static_cast<char>(vk));
    switch (vk) {
        case VK_SPACE: return "Space";
        case VK_RETURN: return "Enter";
        case VK_ESCAPE: return "Esc";
        case VK_TAB: return "Tab";
        case VK_BACK: return "Backspace";
        case VK_DELETE: return "Delete";
        case VK_INSERT: return "Insert";
        case VK_UP: return "Up";
        case VK_DOWN: return "Down";
        case VK_LEFT: return "Left";
        case VK_RIGHT: return "Right";
        case VK_PRIOR: return "PageUp";
        case VK_NEXT: return "PageDown";
        case VK_HOME: return "Home";
        case VK_END: return "End";
        case VK_VOLUME_UP: return "VolumeUp";
        case VK_VOLUME_DOWN: return "VolumeDown";
        case VK_VOLUME_MUTE: return "VolumeMute";
        default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    return {};
}

} // namespace

std::optional<KeySpec> ParseKeySpec(const std::string& text) {
    KeySpec spec;
    bool ctrl = false, alt = false, shift = false, win = false;
    bool has_main = false;

    size_t pos = 0;
    const std::string input = Trim(text);
    while (true) {
        const size_t plus = input.find('+', pos);
        const std::string part = Lower(Trim(input.substr(
            pos, plus == std::string::npos ? std::string::npos : plus - pos)));
        if (part.empty()) return std::nullopt;

        if (part == "ctrl" || part == "control") {
            if (ctrl) return std::nullopt;
            ctrl = true;
        } else if (part == "alt") {
            if (alt) return std::nullopt;
            alt = true;
        } else if (part == "shift") {
            if (shift) return std::nullopt;
            shift = true;
        } else if (part == "win" || part == "windows" || part == "meta") {
            if (win) return std::nullopt;
            win = true;
        } else {
            if (has_main) return std::nullopt;  // 多个主键
            const auto vk = MainKeyVkey(part);
            if (!vk.has_value()) return std::nullopt;
            spec.vk = *vk;
            has_main = true;
        }
        if (plus == std::string::npos) break;
        pos = plus + 1;
    }
    if (!has_main) return std::nullopt;  // 仅修饰键

    // 修饰键固定 Ctrl/Alt/Shift/Win 序，display_text 规范化。
    if (ctrl) spec.modifiers.push_back(VK_CONTROL);
    if (alt) spec.modifiers.push_back(VK_MENU);
    if (shift) spec.modifiers.push_back(VK_SHIFT);
    if (win) spec.modifiers.push_back(VK_LWIN);
    static const char* kModNames[] = {"Ctrl", "Alt", "Shift", "Win"};
    size_t name_idx = 0;
    for (UINT mod : spec.modifiers) {
        if (name_idx > 0) spec.display_text += "+";
        switch (mod) {
            case VK_CONTROL: spec.display_text += kModNames[0]; break;
            case VK_MENU: spec.display_text += kModNames[1]; break;
            case VK_SHIFT: spec.display_text += kModNames[2]; break;
            case VK_LWIN: spec.display_text += kModNames[3]; break;
            default: break;
        }
        ++name_idx;
    }
    if (!spec.display_text.empty()) spec.display_text += "+";
    spec.display_text += VkeyDisplayName(spec.vk);
    return spec;
}

} // namespace voicestick
