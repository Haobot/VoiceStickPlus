// Copyright (c) 2026 Voice Stick contributors. All rights reserved.
//
// 按住说话热键键名解析（本机麦克风模式迭代二）：config [local_asr]
// push_to_talk_key 字符串 → 虚拟键码。按住说话语义要求单键（组合键无法
// 区分「按住说话」与正常打字），仅接受可明确归属的键名。

#pragma once

#include <windows.h>

#include <optional>
#include <string>

namespace voicestick {

// 解析按住说话键名：right/left ctrl|shift|alt、f1-f24、capslock、scrolllock、
// pause、esc、space、tab、enter、backspace、单字符 A-Z 0-9。大小写与前后
// 空白不敏感。歧义（"ctrl" 左右不明）、组合键（"ctrl+c"）、未知键名返回 nullopt。
std::optional<UINT> ParsePushToTalkKey(const std::string& text);

}  // namespace voicestick
