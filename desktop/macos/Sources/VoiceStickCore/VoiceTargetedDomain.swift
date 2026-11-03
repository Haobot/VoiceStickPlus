import Foundation

// 语音编辑批次 2（追问「第几个」+ 整个输入框可编辑）纯逻辑层：
//   VoiceEditNumberParsing   —— 中文/阿拉伯序数解析（consumePosition 与序数应答共用）；
//   VoiceTargetedDomain      —— targeted 搜索域解析规则表（最近一次语音输入 Tier 1
//                               优先 → 整框；显式作用域；位置限定；field_too_long）；
//   VoiceTargetedCursorMath  —— 编辑后光标恢复「原位置 + 平移」。
//
// 序数应答解析见 VoiceEditOrdinalReply.swift（交付 B）。本文件位于 VoiceStickCore，
// 不依赖 AppKit；双端同构：Windows desktop/windows/src/voice_targeted_domain.{h,cc}
// 使用同一张规则表与同一份测试向量（UTF-8 输入、grapheme/UTF-16 口径与
// voice_targeted_edit 一致）。
//
// 域解析只负责「在哪个 haystack 里找、找第几个」；锚定（最近一次语音输入可定位）
// 由协调器完成 VoiceEditAnchor.locate 后以 recent 参数传入——recent == nil 即
// 「最近一次语音输入不可定位」（无记录/过期/换应用/锚定失败），规则表按整框处理
//（显式作用域也退回整框，见 Doc/Plan/voice-edit-commands.md §2.1）。field_scope
// 关闭时协调器走批次 1 原路径，不进入本解析器（回归开关语义）。

/// 整框域字段长度上限（UTF-16 码元）：超过即 `field_too_long`，不在整个输入框里
/// 修改（最近一次语音输入域不受此限，沿用既有 2M 上限）。
public let kVoiceTargetedFieldScopeMaxUTF16 = 200_000

// ---- 序数数字解析（targeted 指令 consumePosition 与追问应答共用）----

/// 中文数字（一…九十九，含「十一」「二十三」「两」）与阿拉伯数字（1–99）解析。
/// 与 VoiceEditParser.parseNumber 同口径（两 → 2），上限由调用方传入（指令位置 99、
/// 裸数字应答 20）。grapheme 前进（阿拉伯数字逐位消费）。
enum VoiceEditNumberParsing {
    private static let chineseDigits: [Character: Int] = [
        "零": 0, "〇": 0, "一": 1, "二": 2, "两": 2,
        "三": 3, "四": 4, "五": 5, "六": 6, "七": 7, "八": 8, "九": 9
    ]

    /// 在 index 处消费一个 1…max 的序数；命中返回值并推进 index，未命中返回 nil
    ///（index 不动）。阿拉伯数字按位累计（超出 max 即失败，不部分消费）；中文数字
    /// 支持 十 / 十Y / X / X十 / X十Y。
    static func consumeOrdinal(_ chars: [Character], _ index: inout Int, max: Int) -> Int? {
        guard index < chars.count else { return nil }
        // 阿拉伯数字（全角已在调用方归一化转半角）。
        if chars[index].isASCII, let digit = chars[index].wholeNumberValue, (0...9).contains(digit) {
            var value = 0
            var consumed = 0
            var probe = index
            while probe < chars.count, chars[probe].isASCII,
                  let next = chars[probe].wholeNumberValue, (0...9).contains(next) {
                value = value * 10 + next
                consumed += 1
                probe += 1
                if value > max { return nil }   // 越界整串不识别（不部分消费）
            }
            guard consumed > 0, value >= 1 else { return nil }
            index = probe
            return value
        }
        // 中文数字。
        var probe = index
        var value = 0
        if chars[probe] == "十" {
            value = 10
            probe += 1
            if probe < chars.count, let unit = chineseDigits[chars[probe]],
               (1...9).contains(unit) {
                value += unit
                probe += 1
            }
        } else if let digit = chineseDigits[chars[probe]], (1...9).contains(digit) {
            value = digit
            probe += 1
            if probe < chars.count, chars[probe] == "十" {
                value = digit * 10
                probe += 1
                if probe < chars.count, let unit = chineseDigits[chars[probe]],
                   (1...9).contains(unit) {
                    value += unit
                    probe += 1
                }
            }
        } else {
            return nil
        }
        guard value >= 1, value <= max else { return nil }
        index = probe
        return value
    }
}

// ---- 搜索域解析（交付 A 规则表）----

/// 搜索域：最近一次语音输入（anchored 段内）或整个输入框。
public enum VoiceTargetedDomainKind: String, Equatable {
    case lastInjection = "last_injection"
    case wholeField = "whole_field"
}

/// 计划时刻可用的「最近一次语音输入」域：L0 批次 1 闸（记录/时间窗/同前台应用）
/// 全部通过且 VoiceEditAnchor.locate 锚定成功时由协调器构造。
public struct VoiceTargetedRecentDomain: Equatable {
    /// LastInjection.remainingText（anchored 段不含已记录换行）。
    public let remainingText: String
    /// 锚定结果（anchorStart/anchorEnd 为传入 fieldText 基准的偏移）。
    public let anchor: VoiceEditAnchor

    public init(remainingText: String, anchor: VoiceEditAnchor) {
        self.remainingText = remainingText
        self.anchor = anchor
    }
}

/// 域解析结果：
///   execute  —— 域内选中一处（occurrence 偏移相对域文本：lastInjection=remaining、
///               wholeField=fieldText）；
///   clarify  —— 未指定位置且域内多处：追问「第几个」（count=域内出现次数）；
///   outOfRange —— 位置序号越界（count=域内出现次数，供「只有 N 处」提示）；
///   failure  —— notFound / fieldTooLong。
public enum VoiceTargetedDomainOutcome: Equatable {
    case execute(domain: VoiceTargetedDomainKind, selected: VoiceTargetedOccurrence)
    case clarify(count: Int, domain: VoiceTargetedDomainKind)
    case outOfRange(count: Int)
    case failure(VoiceTargetedFailureCode)
}

/// 域解析纯函数（规则表全在此，可单测；双端同构）：
///
/// | 指令形态 | 搜索域 |
/// |---|---|
/// | 无作用域、无位置 | Tier 1：recent 可用且其中 X 恰 1 处 → lastInjection；否则整框 |
/// | 显式作用域「刚才的X」 | recent 可用 → lastInjection（X 不在其中 → notFound）；不可用 → 整框 |
/// | 无作用域 + 位置 | 整框（按文档顺序数） |
/// | 显式作用域 + 位置 | recent 可用 → lastInjection；不可用 → 整框 |
///
/// 唯一 → execute；多处 → clarify（该域内编号）；越界 → outOfRange；整框域超长
///（> 200,000 UTF-16）→ fieldTooLong。
public enum VoiceTargetedDomain {
    public static func resolve(
        command: VoiceTargetedCommand,
        recent: VoiceTargetedRecentDomain?,
        fieldText: String
    ) -> VoiceTargetedDomainOutcome {
        // 整框域通用分支：超长保护 → 匹配 → 按位置选择。
        func resolveWholeField() -> VoiceTargetedDomainOutcome {
            guard fieldText.utf16.count <= kVoiceTargetedFieldScopeMaxUTF16 else {
                return .failure(.fieldTooLong)
            }
            let matches = VoiceTargetedMatcher.occurrences(of: command.target, in: fieldText)
            guard !matches.isEmpty else { return .failure(.notFound) }
            switch command.position.select(from: matches) {
            case .selected(let occurrence):
                return .execute(domain: .wholeField, selected: occurrence)
            case .ambiguous(let count):
                return .clarify(count: count, domain: .wholeField)
            case .outOfRange(let count):
                return .outOfRange(count: count)
            }
        }

        // 最近一次语音输入域分支（调用方保证 recent 可用）。
        func resolveRecent(_ domain: VoiceTargetedRecentDomain) -> VoiceTargetedDomainOutcome {
            let matches = VoiceTargetedMatcher.occurrences(
                of: command.target, in: domain.remainingText)
            guard !matches.isEmpty else { return .failure(.notFound) }
            switch command.position.select(from: matches) {
            case .selected(let occurrence):
                return .execute(domain: .lastInjection, selected: occurrence)
            case .ambiguous(let count):
                return .clarify(count: count, domain: .lastInjection)
            case .outOfRange(let count):
                return .outOfRange(count: count)
            }
        }

        if command.hasExplicitScope {
            // 显式作用域：可定位 → 严格限于最近一次语音输入（X 不在其中 → notFound，
            // 不回落——用户明说「刚才的」；被改过/歧义/过期/无记录 → 整框）。
            if let recent { return resolveRecent(recent) }
            return resolveWholeField()
        }
        // 无作用域 + 无位置：Tier 1（recent 可用且恰 1 处）→ 否则整框。
        if case .unspecified = command.position {
            if let recent {
                let matches = VoiceTargetedMatcher.occurrences(
                    of: command.target, in: recent.remainingText)
                if matches.count == 1 {
                    return .execute(domain: .lastInjection, selected: matches[0])
                }
            }
            return resolveWholeField()
        }
        // 无作用域 + 位置限定：整框按文档顺序数（不启用 Tier 1）。
        return resolveWholeField()
    }
}

// ---- 光标恢复平移（交付 C）----

/// 编辑后光标恢复统一为「用户原来的位置 + 平移」（所有域；纯函数）：
///   原光标 ≤ 编辑区间起点 → 不变；
///   原光标 ≥ 编辑区间终点 → 加长度差（inserted - rangeLength）；
///   原光标在编辑区间内 → 编辑后区间末尾（起点 + inserted）。
/// 末尾追加情形（原光标 ≥ 区间终点）结果与批次 1「恢复到段末/字段末尾」完全一致
///（回归底线）；光标在段首/段中（P1b/段中）由「移到段末」改为「留在原位附近」
/// 属批次 2 有意变化（Doc/Plan/voice-edit-commands.md §17）。
public enum VoiceTargetedCursorMath {
    public static func restoredCaret(
        originalCaretUTF16: Int,
        rangeStartUTF16: Int,
        rangeLengthUTF16: Int,
        insertedLengthUTF16: Int
    ) -> Int {
        if originalCaretUTF16 <= rangeStartUTF16 { return originalCaretUTF16 }
        let rangeEnd = rangeStartUTF16 + rangeLengthUTF16
        if originalCaretUTF16 >= rangeEnd {
            return originalCaretUTF16 - rangeLengthUTF16 + insertedLengthUTF16
        }
        return rangeStartUTF16 + insertedLengthUTF16
    }
}

extension VoiceEditNumberParsing {
    /// 全角数字转半角（与 VoiceTargetedParser/VoiceEditParser.normalize 同口径）。
    static func fullWidthDigitsToHalfWidth(_ text: String) -> String {
        var scalars = String.UnicodeScalarView()
        for scalar in text.unicodeScalars {
            if scalar.value >= 0xFF10 && scalar.value <= 0xFF19,
               let ascii = Unicode.Scalar(scalar.value - 0xFF10 + 0x30) {
                scalars.append(ascii)
            } else {
                scalars.append(scalar)
            }
        }
        return String(scalars)
    }
}
