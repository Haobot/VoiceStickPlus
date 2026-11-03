import Foundation

/// 标点独词输入：整轮 ASR final 只说了标点名（如「逗号」「左引号」）时，
/// 直接产出符号本身（「，」「“」）而不是对应文字。对齐 Windows
/// spoken_punctuation.{h,cc}——映射表与判定向量双端 1:1 钉死
///（SpokenPunctuationTests.swift / core_tests_spoken_punctuation.cc）。
public enum SpokenPunctuation {
    /// 标点名 → 符号。成对符号只收「左/右」显式说法（裸「引号」「括号」
    /// 方向歧义，不映射）；英文标点名不收（ASR 为中文引擎）。
    static let symbolsByName: [String: String] = [
        "逗号": "，",
        "逗点": "，",
        "句号": "。",
        "句点": "。",
        "问号": "？",
        "叹号": "！",
        "感叹号": "！",
        "惊叹号": "！",
        "顿号": "、",
        "冒号": "：",
        "分号": "；",
        "省略号": "……",
        "破折号": "——",
        "波浪号": "～",
        "间隔号": "·",
        "左引号": "“",
        "左双引号": "“",
        "右引号": "”",
        "右双引号": "”",
        "左单引号": "‘",
        "右单引号": "’",
        "左括号": "（",
        "右括号": "）",
        "左书名号": "《",
        "右书名号": "》",
    ]

    /// 首尾噪声字符（空白另算）：ASR 常在词后补「。」；波浪号 〜～ 在
    /// Unicode 分类里是符号不是标点，须显式列入。与 Windows
    /// spoken_punctuation.cc 的 kEdgeNoise 同集合。
    private static let edgeNoiseScalars: Set<Unicode.Scalar> = Set(
        "，。、！？：；（）〈〉《》【】“”‘’·…—–～〜.,!?;:()\"'~".unicodeScalars
    )

    /// 整段文本剥掉首尾空白/标点后恰好是一个标点名时返回对应符号；其余
    /// 情况返回 nil 按原文直通——包括文本本身已是纯符号（剥完为空，说明
    /// ASR 已直接上符号，不需要也不应再转换）。
    public static func symbolIfSoleUtterance(_ text: String) -> String? {
        let scalars = Array(text.unicodeScalars)
        var start = 0
        var end = scalars.count
        while start < end, isEdgeNoise(scalars[start]) { start += 1 }
        while end > start, isEdgeNoise(scalars[end - 1]) { end -= 1 }
        guard start < end else { return nil }
        let name = String(String.UnicodeScalarView(scalars[start..<end]))
        return symbolsByName[name]
    }

    private static func isEdgeNoise(_ scalar: Unicode.Scalar) -> Bool {
        scalar.properties.isWhitespace || edgeNoiseScalars.contains(scalar)
    }
}
