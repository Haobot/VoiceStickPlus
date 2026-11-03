import Foundation

// 追问「第几个」的序数应答解析（语音编辑批次 2 交付 B）——纯逻辑层。
// 仅在协调器存在 pendingClarification（20 秒有效）时调用：裸数字误触面因此极小。
// 双端同构：Windows desktop/windows/src/voice_edit_ordinal_reply.{h,cc}（并入
// voice_targeted_domain.cc 实现）使用同一份接受表与测试向量。
//
// 接受：
//   第N个 / 第N处 / 第N / 第N个X（X 与 pending 目标相同时）
//   倒数第N个 / 倒数第N处 / 倒数第N（+ 可选目标词后缀）
//   最后一个 / 最后那个 / 最后一处（→ 倒数第 1，+ 可选目标词后缀）
//   裸数字：中文一…二十、阿拉伯 1–20
// 前缀可带（剥一层，最长优先）：就 / 选 / 是 / 就是 / 选择 / 要 / 改；
// 尾部可带（剥一层，最长优先）：吧 / 啊 / 呀 / 的 / 就行 / 那个；剥尾部标点（可叠加）。
// 取消：取消 / 算了 / 不改了 / 不用了 / 不要了（同样吃前后缀剥离）。
// 其余一律返回 nil = 非应答（按正常链路放行，绝不吞用户的正常说话）。

/// 序数应答解析结果：第 N 个（1 基）/ 倒数第 N 个（1 基）/ 取消。
public enum VoiceEditOrdinalReply: Equatable {
    case ordinal(Int)
    case fromEnd(Int)
    case cancel
}

public enum VoiceEditOrdinalReplyParser {
    private static let prefixes: [String] = ["就是", "选择", "就", "选", "是", "要", "改"]
    private static let suffixes: [String] = ["就行", "那个", "吧", "啊", "呀", "的"]
    private static let trailingPunctuation: Set<Character> = [
        "。", "．", ".", "！", "!", "？", "?", "，", ",", "；", ";", "、"
    ]
    private static let cancelForms: Set<String> = ["取消", "算了", "不改了", "不用了", "不要了"]

    public static func parse(_ text: String, target: String?) -> VoiceEditOrdinalReply? {
        var chars = Array(VoiceEditNumberParsing.fullWidthDigitsToHalfWidth(
            text.trimmingCharacters(in: .whitespacesAndNewlines)))
        // 剥尾部标点（可叠加，每次剥后再去空白）。
        while let last = chars.last, trailingPunctuation.contains(last) {
            chars.removeLast()
            chars = Array(String(chars).trimmingCharacters(in: .whitespacesAndNewlines))
        }
        guard !chars.isEmpty else { return nil }
        // 剥一层前缀（最长优先）与一层尾部语气（最长优先）。
        for prefix in prefixes where hasPrefixToken(chars, prefix) {
            chars.removeFirst(prefix.count)
            break
        }
        for suffix in suffixes where hasSuffixToken(chars, suffix) {
            chars.removeLast(suffix.count)
            break
        }
        chars = Array(String(chars).trimmingCharacters(in: .whitespacesAndNewlines))
        guard !chars.isEmpty else { return nil }

        // 取消类（整词精确匹配）。
        if cancelForms.contains(String(chars)) { return .cancel }

        // 倒数第 N 个/处/N（+ 可选目标词后缀）。
        var index = 0
        if matchToken(chars, &index, "倒数第"),
           let n = VoiceEditNumberParsing.consumeOrdinal(chars, &index, max: 99),
           matchUnitAndOptionalTarget(chars, &index, target: target) {
            return .fromEnd(n)
        }
        // 最后一个 / 最后那个 / 最后一处（→ fromEnd(1)，+ 可选目标词后缀）。
        //「最后那个」会先被尾部语气剥成裸「最后」——同样按 fromEnd(1) 接受。
        index = 0
        if matchToken(chars, &index, "最后"),
           matchToken(chars, &index, "一个") || matchToken(chars, &index, "那个")
           || matchToken(chars, &index, "一处"),
           matchOptionalTarget(chars, &index, target: target) {
            return .fromEnd(1)
        }
        if chars.count == 2, chars[0] == "最", chars[1] == "后" {
            return .fromEnd(1)
        }
        // 第 N 个/处/N（+ 可选目标词后缀）。
        index = 0
        if matchToken(chars, &index, "第"),
           let n = VoiceEditNumberParsing.consumeOrdinal(chars, &index, max: 99),
           matchUnitAndOptionalTarget(chars, &index, target: target) {
            return .ordinal(n)
        }
        // 裸数字（中文一…二十 / 阿拉伯 1–20；须整串恰为数字）。
        index = 0
        if let n = VoiceEditNumberParsing.consumeOrdinal(chars, &index, max: 20),
           index == chars.count {
            return .ordinal(n)
        }
        return nil
    }

    /// 单位（个/处，可省，至多一个）+ 可选目标词（须与 pending 目标完全一致）。
    private static func matchUnitAndOptionalTarget(
        _ chars: [Character], _ index: inout Int, target: String?
    ) -> Bool {
        if matchToken(chars, &index, "个") || matchToken(chars, &index, "处") {
            return matchOptionalTarget(chars, &index, target: target)
        }
        return matchOptionalTarget(chars, &index, target: target)
    }

    /// 剩余部分为空，或恰为 pending 目标词（「第二个北京」）。
    private static func matchOptionalTarget(
        _ chars: [Character], _ index: inout Int, target: String?
    ) -> Bool {
        guard index < chars.count else { return true }
        guard let target, !target.isEmpty else { return false }
        var probe = index
        guard matchToken(chars, &probe, target), probe == chars.count else { return false }
        index = probe
        return true
    }

    // ---- 词法工具（与 VoiceTargetedParser 同口径，作用域独立）----

    private static func hasPrefixToken(_ chars: [Character], _ token: String) -> Bool {
        let tokenChars = Array(token)
        guard chars.count >= tokenChars.count else { return false }
        for offset in 0..<tokenChars.count where chars[offset] != tokenChars[offset] {
            return false
        }
        return true
    }

    private static func hasSuffixToken(_ chars: [Character], _ token: String) -> Bool {
        let tokenChars = Array(token)
        guard chars.count >= tokenChars.count else { return false }
        for offset in 0..<tokenChars.count
        where chars[chars.count - tokenChars.count + offset] != tokenChars[offset] {
            return false
        }
        return true
    }

    private static func matchToken(_ chars: [Character], _ index: inout Int, _ token: String) -> Bool {
        let tokenChars = Array(token)
        guard chars.count - index >= tokenChars.count else { return false }
        for offset in 0..<tokenChars.count where chars[index + offset] != tokenChars[offset] {
            return false
        }
        index += tokenChars.count
        return true
    }
}
