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

}  // namespace

std::optional<UINT> ParsePushToTalkKey(const std::string& text) {
    // 键名为 Normalize 后形态（去全部空白 + 小写），如 "right ctrl"→"rightctrl"。
    static const struct {
        const char* name;
        UINT vk;
    } kNamedKeys[] = {
        {"rightctrl", VK_RCONTROL},
        {"leftctrl", VK_LCONTROL},
        {"rightshift", VK_RSHIFT},
        {"leftshift", VK_LSHIFT},
        {"rightalt", VK_RMENU},
        {"leftalt", VK_LMENU},
        {"capslock", VK_CAPITAL},
        {"scrolllock", VK_SCROLL},
        {"pause", VK_PAUSE},
        {"esc", VK_ESCAPE},
        {"escape", VK_ESCAPE},
        {"space", VK_SPACE},
        {"tab", VK_TAB},
        {"enter", VK_RETURN},
        {"return", VK_RETURN},
        {"backspace", VK_BACK},
    };

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

}  // namespace voicestick
