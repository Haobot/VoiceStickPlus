// 标点独词输入批（spoken_punctuation.{h,cc}，双端同向量——macOS
// Tests/VoiceStickTests/SpokenPunctuationTests.swift 钉同一组期望值）。
#include "test_suites.h"

#include "spoken_punctuation.h"

#include <cassert>
#include <optional>
#include <string>

using voicestick::SpokenPunctuationOnlySymbol;

namespace {

void ExpectSymbol(const char* text, const char* symbol) {
    const auto got = SpokenPunctuationOnlySymbol(text);
    assert(got.has_value());
    assert(*got == symbol);
}

void ExpectPassthrough(const char* text) {
    assert(!SpokenPunctuationOnlySymbol(text).has_value());
}

} // namespace

void TestSpokenPunctuationTable() {
    // 基础映射表。
    ExpectSymbol("逗号", "，");
    ExpectSymbol("逗点", "，");
    ExpectSymbol("句号", "。");
    ExpectSymbol("句点", "。");
    ExpectSymbol("问号", "？");
    ExpectSymbol("叹号", "！");
    ExpectSymbol("感叹号", "！");
    ExpectSymbol("惊叹号", "！");
    ExpectSymbol("顿号", "、");
    ExpectSymbol("冒号", "：");
    ExpectSymbol("分号", "；");
    ExpectSymbol("省略号", "……");
    ExpectSymbol("破折号", "——");
    ExpectSymbol("波浪号", "～");
    ExpectSymbol("间隔号", "·");
    // 成对符号：仅左/右显式说法映射，方向不同符号不同。
    ExpectSymbol("左引号", "“");
    ExpectSymbol("右引号", "”");
    ExpectSymbol("左双引号", "“");
    ExpectSymbol("右双引号", "”");
    ExpectSymbol("左单引号", "‘");
    ExpectSymbol("右单引号", "’");
    ExpectSymbol("左括号", "（");
    ExpectSymbol("右括号", "）");
    ExpectSymbol("左书名号", "《");
    ExpectSymbol("右书名号", "》");
}

void TestSpokenPunctuationEdgeNoise() {
    // 首尾噪声剥离（ASR 常在词后补句号/带首尾空白）。
    ExpectSymbol("逗号。", "，");
    ExpectSymbol("问号？", "？");
    ExpectSymbol(" 逗号 ", "，");
    ExpectSymbol("，逗号，", "，");
    ExpectSymbol("省略号……", "……");
    ExpectSymbol("波浪号～", "～");  // 波浪号是符号类不是标点类，须显式列入噪声集
}

void TestSpokenPunctuationPassthrough() {
    // 非独词不转换。
    ExpectPassthrough("你好逗号");
    ExpectPassthrough("逗号你好");
    ExpectPassthrough("逗号句号");
    ExpectPassthrough("问号吗");
    // 已是纯符号（ASR 直接上符号）：剥完为空 → 直通。
    ExpectPassthrough("，");
    ExpectPassthrough("。");
    ExpectPassthrough("……");
    ExpectPassthrough("你好，世界。");
    // 成对符号裸说法方向歧义，不映射。
    ExpectPassthrough("引号");
    ExpectPassthrough("括号");
    ExpectPassthrough("书名号");
    // 空文本/纯空白/单字不成名。
    ExpectPassthrough("");
    ExpectPassthrough("   ");
    ExpectPassthrough("逗");
}

void RunSpokenPunctuationBatchTests() {
    TestSpokenPunctuationTable();
    TestSpokenPunctuationEdgeNoise();
    TestSpokenPunctuationPassthrough();
}
