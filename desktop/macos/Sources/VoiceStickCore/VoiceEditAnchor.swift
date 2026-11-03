import Foundation

// 字段内定位器（语音编辑批次 1：输入框内定位 P1 + 句中插入去句号）——纯逻辑层。
// 对应 Doc/Plan/voice-edit-commands.md §17（批次 1 实施记录）。
//
// 背景：旧语音编辑链假设「听写追加在字段末尾且其后无内容」（targeted L1 要求完整
// remainingText 是字段后缀、尾删要求 caret==字段末尾），句中插入 / 插入点后有字时
// 恒 field_mismatch / caret_not_end。本文件把「定位」升级为字段内多级锚定：
//
//   P1a 光标前缀锚：selectionLength==0 且字段前缀 [0, caret) 以 anchored 结尾；
//   P1b 光标后缀锚：selectionLength==0 且字段后缀 [caret, len) 以 anchored 开头；
//   P1c 字段末尾锚：anchored 是字段后缀（含现行「remaining+已记录\n」优先判语义）；
//   P2  唯一出现锚：anchored 在字段中非重叠精确出现恰好一次（≥2 次 anchor_ambiguous）。
//
// 判定顺序固定 P1a → P1b → P1c → P2，零容错：禁止任何模糊/最长前缀匹配（旧尾删的
// 最长前缀容错在本批次移除——用户改动后偏移不可靠，宁拒勿错）。匹配口径全部
// UTF-16 码元精确相等（与平台选区 API 同口径），emoji/组合字符不拆 surrogate。
//
// 本文件位于 VoiceStickCore，不依赖 AppKit；双端同构：Windows
// desktop/windows/src/voice_edit_anchor.{h,cc} 使用同一份判定顺序与测试向量
//（UTF-8 输入、内部换算 UTF-16 偏移）。
//
// 另含交付 C 的纯函数：句中插入去句末句号（stripMidlinePeriod）与句中判定
//（isMidlineInsertion），双端同构 StripMidlinePeriod/IsMidlineInsertion。

/// 锚定结果：anchored 段在字段中的位置（UTF-16 偏移）。
public struct VoiceEditAnchor: Equatable {
    public enum Kind: Equatable {
        case caretPrefix
        case caretSuffix
        case fieldEnd
        case uniqueOccurrence
    }

    public let kind: Kind
    /// anchored 段在字段中的起始 UTF-16 偏移。
    public let anchorStart: Int
    /// = anchorStart + anchored.utf16.count（P1c 裸后缀回落时 = remaining 长度）。
    public let anchorEnd: Int

    public init(kind: Kind, anchorStart: Int, anchorEnd: Int) {
        self.kind = kind
        self.anchorStart = anchorStart
        self.anchorEnd = anchorEnd
    }
}

/// 定位失败码（raw = 日志用 code；用户提示文案由 App 层 L10nKey 映射）。
public enum VoiceEditAnchorCode: String, Equatable {
    case anchorAmbiguous = "anchor_ambiguous"
    case fieldMismatch = "field_mismatch"
}

public enum VoiceEditAnchorResult: Equatable {
    case anchored(VoiceEditAnchor)
    case failure(VoiceEditAnchorCode)
}

extension VoiceEditAnchor {
    /// 字段内多级锚定。调用方保证 fieldText/caretUTF16/selectionLength 可读
    ///（读不到 = 不可验证，在进入本函数前已按各自通道语义拒绝）；remaining
    /// 非空（LastInjection 通道已保证）。anchored（remaining + 已记录换行）由
    /// 本函数按 pressedEnter 构造，与双端 C++ 侧「调用方构造好传入」的口径一致。
    public static func locate(
        remaining: String, pressedEnter: Bool,
        fieldText: String, caretUTF16: Int, selectionLength: Int
    ) -> VoiceEditAnchorResult {
        let field = Array(fieldText.utf16)
        let anchored = Array((pressedEnter ? remaining + "\n" : remaining).utf16)
        // 空 anchored：不可能出现（调用方保证 remaining 非空；防御，按全 miss 处理）。
        // 注意 anchored 比：字段长**不是**拒绝条件——P1c 裸回落（用户已手删已记录
        // 换行）仍可能命中，各锚级自带的边界守卫自然处理越界。
        guard !anchored.isEmpty else { return .failure(.fieldMismatch) }

        // P1a 光标前缀锚：anchored 紧贴光标之前。
        if selectionLength == 0,
           caretUTF16 >= anchored.count, caretUTF16 <= field.count,
           unitsEqual(field, at: caretUTF16 - anchored.count, anchored) {
            return .anchored(VoiceEditAnchor(
                kind: .caretPrefix,
                anchorStart: caretUTF16 - anchored.count,
                anchorEnd: caretUTF16
            ))
        }
        // P1b 光标后缀锚：anchored 紧贴光标之后。
        if selectionLength == 0,
           caretUTF16 >= 0, caretUTF16 + anchored.count <= field.count,
           unitsEqual(field, at: caretUTF16, anchored) {
            return .anchored(VoiceEditAnchor(
                kind: .caretSuffix,
                anchorStart: caretUTF16,
                anchorEnd: caretUTF16 + anchored.count
            ))
        }
        // P1c 字段末尾锚（含现行「remaining+\n 优先、裸 remaining 回落」语义）。
        if let fieldEnd = locateFieldEnd(
            remaining: remaining, pressedEnter: pressedEnter, fieldText: fieldText
        ) {
            return .anchored(fieldEnd)
        }
        // P2 唯一出现锚：非重叠精确出现恰好一次；≥2 次 → anchor_ambiguous。
        var hits = 0
        var firstStart = -1
        var index = 0
        while index + anchored.count <= field.count {
            if unitsEqual(field, at: index, anchored) {
                hits += 1
                if hits == 1 { firstStart = index }
                if hits >= 2 { break }
                index += anchored.count  // 非重叠：前进过整个匹配
                continue
            }
            index += 1
        }
        if hits == 1 {
            return .anchored(VoiceEditAnchor(
                kind: .uniqueOccurrence,
                anchorStart: firstStart,
                anchorEnd: firstStart + anchored.count
            ))
        }
        if hits >= 2 { return .failure(.anchorAmbiguous) }
        return .failure(.fieldMismatch)
    }

    /// P1c 字段末尾锚（现行后缀校验的等价实现，供定位器 P1c 步与旧 planner 通道
    /// 共用）：pressedEnter 时优先按「remaining+\n」判后缀（避免 remaining 自身以
    /// 换行结尾时误配裸后缀），回落裸 remaining（用户已手删换行）。命中时
    /// anchorEnd 恒为字段末尾；带换行命中的锚段长度 = remaining+1（含已记录换行），
    /// 裸命中的锚段 = remaining 本身——调用方据此推导 hasRecordedNewline。
    public static func locateFieldEnd(
        remaining: String, pressedEnter: Bool, fieldText: String
    ) -> VoiceEditAnchor? {
        let field = Array(fieldText.utf16)
        let remainingUnits = Array(remaining.utf16)
        guard !remainingUnits.isEmpty, remainingUnits.count <= field.count else { return nil }
        if pressedEnter, remainingUnits.count + 1 <= field.count,
           field[field.count - 1] == 0x0A,
           unitsEqual(field, at: field.count - remainingUnits.count - 1, remainingUnits) {
            return VoiceEditAnchor(
                kind: .fieldEnd,
                anchorStart: field.count - remainingUnits.count - 1,
                anchorEnd: field.count
            )
        }
        guard unitsEqual(field, at: field.count - remainingUnits.count, remainingUnits) else {
            return nil
        }
        return VoiceEditAnchor(
            kind: .fieldEnd,
            anchorStart: field.count - remainingUnits.count,
            anchorEnd: field.count
        )
    }

    /// haystack[start, start+needle.count) 与 needle 的 UTF-16 码元精确相等。
    private static func unitsEqual(
        _ haystack: [UInt16], at start: Int, _ needle: [UInt16]
    ) -> Bool {
        guard start >= 0, start + needle.count <= haystack.count else { return false }
        for offset in 0..<needle.count where haystack[start + offset] != needle[offset] {
            return false
        }
        return true
    }
}

// ---- 交付 C：句中插入去句末句号（纯函数；双端同构 StripMidlinePeriod）----

extension VoiceEditAnchor {
    /// 句中插入判定：光标（折叠）之后**同一逻辑行**还有非空白字符——即光标后第一个
    /// 字符存在、不是 \n/\r、非空白。读不到字段时调用方不进入本判定（原样注入）。
    public static func isMidlineInsertion(fieldText: String, caretUTF16: Int) -> Bool {
        guard caretUTF16 >= 0 else { return false }
        let units = Array(fieldText.utf16)
        guard caretUTF16 < units.count else { return false }
        let rest = String(decoding: units[caretUTF16...], as: UTF16.self)
        guard let first = rest.first else { return false }
        if first == "\n" || first == "\r" { return false }
        return !first.isWhitespace
    }

    /// 快速预检：文本以可剥离候选句号结尾（协调器据此避免不必要的字段读取）。
    /// 语义与 stripMidlinePeriod 的「会不会剥」判定一致（非句中时调用方不进入）。
    public static func midlinePeriodStrippable(_ text: String) -> Bool {
        guard let last = text.last else { return false }
        if last == "。" || last == "．" { return true }
        if last == "." {
            if let before = text.dropLast().last,
               before.isASCII, before.isLetter || before.isNumber {
                return false  // 「3.」「v1.」：恒保留，无需读字段
            }
            return true
        }
        return false
    }

    /// 句中插入且注入文本以一个句号结尾 → 去掉末尾一个字符。句号集合：
    /// 。(U+3002)、．(U+FF0E) 直接剥；.(U+002E) 仅当前一字符不是 ASCII 字母/数字
    ///（保「3.」「v1.」这类数字/版本号）；！？等语气标点不去。auto_enter 换行不受
    /// 影响（本函数只剥句号字符，换行由调用方在 paste 通道处理）。
    public static func stripMidlinePeriod(text: String, isMidline: Bool) -> String {
        guard isMidline, let last = text.last else { return text }
        if last == "。" || last == "．" {
            return String(text.dropLast())
        }
        if last == "." {
            if let before = text.dropLast().last,
               before.isASCII, before.isLetter || before.isNumber {
                return text  // 「3.」「v1.」：句号属于数字/版本号，保留
            }
            return String(text.dropLast())
        }
        return text
    }
}
