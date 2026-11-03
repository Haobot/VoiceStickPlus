#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace voicestick {

// 标点独词判定（纯函数）：整段文本（剥掉首尾空白/标点后）恰好是一个
// 标点名（如“逗号”“左引号”）时返回对应符号，否则返回 std::nullopt。
// 已是纯符号的文本同样返回 nullopt（剥完为空，按原文直通——ASR 直接
// 上符号时不需要也不应再转换）。与 macOS VoiceStickCore/SpokenPunctuation.swift
// 同表同语义，判定向量双端 1:1 钉死（core_tests_spoken_punctuation.cc）。
std::optional<std::string> SpokenPunctuationOnlySymbol(std::string_view text);

} // namespace voicestick
