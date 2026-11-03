import Foundation
import VoiceStickCore

// 标点独词输入单测（与 desktop/windows/tests/core_tests_spoken_punctuation.cc
// 同向量钉死——映射表、首尾噪声剥离、非独词直通语义双端一致）。

func runSpokenPunctuationTests() {
    // ---- 基础映射表 ----
    checkEqual(SpokenPunctuation.symbolIfSoleUtterance("逗号"), "，", "标点独词：逗号")
    checkEqual(SpokenPunctuation.symbolIfSoleUtterance("逗点"), "，", "标点独词：逗点同义")
    checkEqual(SpokenPunctuation.symbolIfSoleUtterance("句号"), "。", "标点独词：句号")
    checkEqual(SpokenPunctuation.symbolIfSoleUtterance("句点"), "。", "标点独词：句点同义")
    checkEqual(SpokenPunctuation.symbolIfSoleUtterance("问号"), "？", "标点独词：问号")
    checkEqual(SpokenPunctuation.symbolIfSoleUtterance("叹号"), "！", "标点独词：叹号")
    checkEqual(SpokenPunctuation.symbolIfSoleUtterance("感叹号"), "！", "标点独词：感叹号同义")
    checkEqual(SpokenPunctuation.symbolIfSoleUtterance("惊叹号"), "！", "标点独词：惊叹号同义")
    checkEqual(SpokenPunctuation.symbolIfSoleUtterance("顿号"), "、", "标点独词：顿号")
    checkEqual(SpokenPunctuation.symbolIfSoleUtterance("冒号"), "：", "标点独词：冒号")
    checkEqual(SpokenPunctuation.symbolIfSoleUtterance("分号"), "；", "标点独词：分号")
    checkEqual(SpokenPunctuation.symbolIfSoleUtterance("省略号"), "……", "标点独词：省略号两字符")
    checkEqual(SpokenPunctuation.symbolIfSoleUtterance("破折号"), "——", "标点独词：破折号两字符")
    checkEqual(SpokenPunctuation.symbolIfSoleUtterance("波浪号"), "～", "标点独词：波浪号")
    checkEqual(SpokenPunctuation.symbolIfSoleUtterance("间隔号"), "·", "标点独词：间隔号")
    // 成对符号：仅左/右显式说法映射，方向不同符号不同。
    checkEqual(SpokenPunctuation.symbolIfSoleUtterance("左引号"), "“", "标点独词：左引号")
    checkEqual(SpokenPunctuation.symbolIfSoleUtterance("右引号"), "”", "标点独词：右引号")
    checkEqual(SpokenPunctuation.symbolIfSoleUtterance("左双引号"), "“", "标点独词：左双引号同义")
    checkEqual(SpokenPunctuation.symbolIfSoleUtterance("右双引号"), "”", "标点独词：右双引号同义")
    checkEqual(SpokenPunctuation.symbolIfSoleUtterance("左单引号"), "‘", "标点独词：左单引号")
    checkEqual(SpokenPunctuation.symbolIfSoleUtterance("右单引号"), "’", "标点独词：右单引号")
    checkEqual(SpokenPunctuation.symbolIfSoleUtterance("左括号"), "（", "标点独词：左括号")
    checkEqual(SpokenPunctuation.symbolIfSoleUtterance("右括号"), "）", "标点独词：右括号")
    checkEqual(SpokenPunctuation.symbolIfSoleUtterance("左书名号"), "《", "标点独词：左书名号")
    checkEqual(SpokenPunctuation.symbolIfSoleUtterance("右书名号"), "》", "标点独词：右书名号")

    // ---- 首尾噪声剥离（ASR 常在词后补句号/带首尾空白）----
    checkEqual(SpokenPunctuation.symbolIfSoleUtterance("逗号。"), "，", "标点独词：尾句号剥离")
    checkEqual(SpokenPunctuation.symbolIfSoleUtterance("问号？"), "？", "标点独词：尾同符号剥离")
    checkEqual(SpokenPunctuation.symbolIfSoleUtterance(" 逗号 "), "，", "标点独词：首尾空白剥离")
    checkEqual(SpokenPunctuation.symbolIfSoleUtterance("，逗号，"), "，", "标点独词：首尾标点剥离")
    checkEqual(SpokenPunctuation.symbolIfSoleUtterance("省略号……"), "……", "标点独词：尾省略号剥离")
    checkEqual(SpokenPunctuation.symbolIfSoleUtterance("波浪号～"), "～", "标点独词：尾波浪号剥离（符号类）")

    // ---- 非独词 / 不转换语义 ----
    check(
        SpokenPunctuation.symbolIfSoleUtterance("你好逗号") == nil,
        "标点独词：句中嵌入不转换"
    )
    check(
        SpokenPunctuation.symbolIfSoleUtterance("逗号你好") == nil,
        "标点独词：后接文字不转换"
    )
    check(
        SpokenPunctuation.symbolIfSoleUtterance("逗号句号") == nil,
        "标点独词：两个标点名并列不转换"
    )
    check(
        SpokenPunctuation.symbolIfSoleUtterance("问号吗") == nil,
        "标点独词：带语气词不转换"
    )
    // 已是纯符号（ASR 直接上符号）：剥完为空 → 直通。
    check(SpokenPunctuation.symbolIfSoleUtterance("，") == nil, "标点独词：已是符号直通")
    check(SpokenPunctuation.symbolIfSoleUtterance("。") == nil, "标点独词：已是句号直通")
    check(SpokenPunctuation.symbolIfSoleUtterance("……") == nil, "标点独词：已是省略号直通")
    check(
        SpokenPunctuation.symbolIfSoleUtterance("你好，世界。") == nil,
        "标点独词：普通句子直通"
    )
    // 成对符号裸说法方向歧义，不映射。
    check(SpokenPunctuation.symbolIfSoleUtterance("引号") == nil, "标点独词：裸引号歧义不映射")
    check(SpokenPunctuation.symbolIfSoleUtterance("括号") == nil, "标点独词：裸括号歧义不映射")
    check(SpokenPunctuation.symbolIfSoleUtterance("书名号") == nil, "标点独词：裸书名号歧义不映射")
    // 空文本/纯空白/单字不成名。
    check(SpokenPunctuation.symbolIfSoleUtterance("") == nil, "标点独词：空文本直通")
    check(SpokenPunctuation.symbolIfSoleUtterance("   ") == nil, "标点独词：纯空白直通")
    check(SpokenPunctuation.symbolIfSoleUtterance("逗") == nil, "标点独词：单字不成名")
}
