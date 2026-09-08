// Copyright (c) 2026 Voice Stick contributors. All rights reserved.

#include "push_to_talk_key.h"

#include <algorithm>
#include <cctype>

namespace voicestick {
namespace {

std::string Normalize(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

// 单字符 A-Z / 0-9：键名即字符本身。
bool ParseCharKey(const std::string& name, UINT& vk) {
    if (name.size() != 1) return false;
    const char c = name[0];
    if (c >= 'a' && c <= 'z') {
        vk = static_cast<UINT>(c - 'a' + 'A');
        return true;
    }
    if (c >= '0' && c <= '9') {
        vk = static_cast<UINT>(c);
        return true;
    }
    return false;
}

// f1-f24：长度校验后解析数字，避免 "f" / "f0" / "f25" / "fx" 误收。
bool ParseFunctionKey(const std::string& name, UINT& vk) {
    if (name.size() < 2 || name[0] != 'f') return false;
    int number = 0;
    for (std::size_t i = 1; i < name.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(name[i]))) return false;
        number = number * 10 + (name[i] - '0');
    }
    if (number < 1 || number > 24) return false;
    vk = VK_F1 + static_cast<UINT>(number) - 1;
    return true;
}

// 命名键表（Parse 与 Format 共用；同义名 escape/return 排在主名后，Format 取首见主名）。
// name = Normalize 形态（去空白小写，Parse 匹配）；display = 规范键名（Format 回显）。
const struct {
    const char* name;
    const char* display;
    UINT vk;
} kNamedKeys[] = {
    {"rightctrl", "right ctrl", VK_RCONTROL},
    {"leftctrl", "left ctrl", VK_LCONTROL},
    {"rightshift", "right shift", VK_RSHIFT},
    {"leftshift", "left shift", VK_LSHIFT},
    {"rightalt", "right alt", VK_RMENU},
    {"leftalt", "left alt", VK_LMENU},
    {"capslock", "capslock", VK_CAPITAL},
    {"scrolllock", "scrolllock", VK_SCROLL},
    {"pause", "pause", VK_PAUSE},
    {"esc", "esc", VK_ESCAPE},
    {"escape", "esc", VK_ESCAPE},
    {"space", "space", VK_SPACE},
    {"tab", "tab", VK_TAB},
    {"enter", "enter", VK_RETURN},
    {"return", "enter", VK_RETURN},
    {"backspace", "backspace", VK_BACK},
};

}  // namespace

std::optional<UINT> ParsePushToTalkKey(const std::string& text) {
    // 键名为 Normalize 后形态（去全部空白 + 小写），如 "right ctrl"→"rightctrl"。
    const std::string name = Normalize(text);
    if (name.empty()) return std::nullopt;
    for (const auto& key : kNamedKeys) {
        if (name == key.name) return key.vk;
    }
    UINT vk = 0;
    if (ParseFunctionKey(name, vk)) return vk;
    if (ParseCharKey(name, vk)) return vk;
    return std::nullopt;
}

std::optional<std::string> FormatPushToTalkKey(UINT vk) {
    for (const auto& key : kNamedKeys) {
        if (vk == key.vk) return key.display;  // 首见即主名（esc/enter）
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        return "f" + std::to_string(vk - VK_F1 + 1);
    }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        // 单字符键名即小写字符本身（Parse 侧 a-z/0-9 对应 VK 'A'-'Z'/'0'-'9'）。
        return std::string(1, static_cast<char>(std::tolower(static_cast<unsigned char>(vk))));
    }
    return std::nullopt;
}

}  // namespace voicestick
