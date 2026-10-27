import Foundation

// Targeted 语音编辑（Voice Targeted Edit）纯逻辑层——「删除/替换最近一次语音输入中
// 的指定词/短语」。对应 Doc/Plan/voice-edit-commands.md targeted 扩展章节。
// 本文件位于 VoiceStickCore，不依赖 AppKit；双端同构：Windows
// voice_targeted_edit.{h,cc} 使用同一张语法表与同一份测试向量。
//
// 与旧 VoiceEditParser（字/句/整段尾部删除）的关系：旧解析器优先，本解析器其次；
// 两者互斥——旧指令不带操作数，targeted 指令必须带「刚才的」作用域 + 操作数。
//
// 职责边界：
//   VoiceTargetedParser —— ASR final 文本 → 结构化指令或「结构命中但操作数非法」；
//   VoiceTargetedMatcher —— remainingText 内目标词非重叠精确匹配（grapheme 区间 +
//                             UTF-16 偏移）；不做任何 IO；
//   VoiceTargetedPlanner —— 指令 + 匹配结果 + 焦点字段/光标状态 → 执行计划或拒绝码。
// L0 作用域闸（enabled / focused_app / 时间窗 / 前台应用 / AX 权限）由协调器负责。

/// 目标词长度上限（grapheme，双端一致常量；独立于 [voice_edit].max_chars）。
public let kVoiceTargetedMaxTargetGraphemes = 32
/// 替换文本长度上限（grapheme，双端一致常量）。
public let kVoiceTargetedMaxReplacementGraphemes = 64
/// 指令文本长度上限（grapheme）：作用域词 + 动词/分隔符 + 两个操作数 + 礼貌/语气余量。
public let kVoiceTargetedMaxCommandGraphemes = 128

// ---- 数据结构（两端同构）----

/// targeted 动作：删除目标词，或替换为 replacement。
public enum VoiceTargetedAction: Equatable {
    case delete
    case replace(String)
}

/// 位置限定：第一个 / 最后一个 / 未指定（仅 1 个精确匹配才允许未指定）。
public enum VoiceTargetedPosition: Equatable {
    case first
    case last
    case unspecified
}

/// 解析结果（操作数已剥首尾空白，内部有意义空白保留）。
public struct VoiceTargetedCommand: Equatable {
    public let action: VoiceTargetedAction
    public let target: String
    public let position: VoiceTargetedPosition

    public init(action: VoiceTargetedAction, target: String, position: VoiceTargetedPosition) {
        self.action = action
        self.target = target
        self.position = position
    }
}

/// 解析三态：指令 / 结构命中但操作数非法（吞掉并提示，不上屏）/ 非指令（走原链路）。
public enum VoiceTargetedParseResult: Equatable {
    case command(VoiceTargetedCommand)
    case invalid
}

/// 失败码（raw = 日志用 code；用户提示文案由 App 层 L10nKey 映射）。
public enum VoiceTargetedFailureCode: String, CaseIterable, Equatable {
    // L1/解析层。
    case notFound = "target_not_found"
    case ambiguous = "target_ambiguous"
    case invalid = "target_invalid"
    case fieldMismatch = "field_mismatch"
    case caretNotEnd = "caret_not_end"
    case selectionPresent = "selection_present"
    case verifyUnavailable = "verify_unavailable"
    case resultUnconfirmed = "result_unconfirmed"
    // L0 作用域层（与旧 VoiceEditFailureCode 同名同语义；toast 复用旧文案 key）。
    case noInjection = "no_injection"
    case scopeExpired = "scope_expired"
    case appChanged = "app_changed"
    case accessibilityDenied = "accessibility_denied"
}

/// remainingText 内的一次非重叠精确匹配。
public struct VoiceTargetedOccurrence: Equatable {
    /// 第几个（0-based；第一个 = 0，最后一个 = total - 1）。
    public let index: Int
    /// 非重叠匹配总数。
    public let total: Int
    /// target 在 remainingText 中的 grapheme 区间（end 为开区间）。
    public let graphemeRange: Range<Int>
    /// target 在 remainingText.utf16 中的偏移与长度（平台选区用）。
    public let utf16Start: Int
    public let utf16Length: Int

    public init(index: Int, total: Int, graphemeRange: Range<Int>,
                utf16Start: Int, utf16Length: Int) {
        self.index = index
        self.total = total
        self.graphemeRange = graphemeRange
        self.utf16Start = utf16Start
        self.utf16Length = utf16Length
    }
}

/// 执行计划：选区区间（remainingText 内）+ 执行后的 newRemainingText。
public struct VoiceTargetedPlan: Equatable {
    public let action: VoiceTargetedAction
    /// 目标词 grapheme 数（成功 toast / 日志用，不写内容）。
    public let targetLength: Int
    public let occurrence: VoiceTargetedOccurrence
    /// 执行后 LastInjection.remainingText（删除=摘除目标区间；替换=插入 replacement）。
    public let newRemainingText: String

    public init(action: VoiceTargetedAction, targetLength: Int,
                occurrence: VoiceTargetedOccurrence, newRemainingText: String) {
        self.action = action
        self.targetLength = targetLength
        self.occurrence = occurrence
        self.newRemainingText = newRemainingText
    }
}

public enum VoiceTargetedPlanResult: Equatable {
    case plan(VoiceTargetedPlan)
    case failure(VoiceTargetedFailureCode)
}

// ---- 解析器 ----

/// 中文整句白名单解析器：只接受「整句恰好是一条带『刚才的』作用域的删除/替换指令」。
/// 纯函数，无状态。词按连续文本片段精确匹配，不引入分词依赖。
public enum VoiceTargetedParser {
    /// 作用域标记（必须出现且仅出现一次，位于动词之后或把字句的「把」之后）。
    private static let scopeTokens: [String] = ["刚才的", "刚才", "刚刚的", "刚刚"]
    /// 位置限定词。
    private static let positionFirst = "第一个"
    private static let positionLast = "最后一个"
    /// 前行动词（删除式）：动宾式「VERB 刚才的 …」。
    private static let leadingDeleteVerbs: [String] = ["删除", "删掉", "删去", "去掉"]
    /// 后置删除动词（把字句尾部）。
    private static let trailingDeleteVerbs: [String] = ["删掉", "删除", "去掉", "删去"]
    /// 替换分隔符（把字句尾部，replacement 为其余全部内容）。
    private static let replaceSeparators: [String] = ["替换成", "替换为", "改成", "改为", "换成"]
    /// 礼貌前缀（仅剥离绝对开头一层，最长匹配优先）。
    private static let politePrefixes: [String] = ["请帮我", "麻烦帮我", "帮我", "麻烦", "请"]
    /// 语气尾（仅剥离绝对结尾一层；剥后命中优先，与旧解析器同口径）。
    private static let trailingParticles: Set<Character> = ["吧", "啊", "呀", "哦", "嘛"]
    private static let trailingPunctuation: Set<Character> = [
        "。", "．", ".", "！", "!", "？", "?", "，", ",", "；", ";", "、"
    ]

    /// 整句解析。
    /// - Parameter text: ASR final 原文（可含首尾空白/尾部标点/全角数字/礼貌前缀/语气尾）。
    /// - Returns: `.command` = 可执行指令；`.invalid` = 结构命中但操作数非法（空目标/
    ///   空替换/多替换分隔符/超长），调用方吞掉并提示；nil = 非指令（按普通文本上屏）。
    public static func parse(_ text: String) -> VoiceTargetedParseResult? {
        let normalized = normalize(text)
        guard !normalized.isEmpty, normalized.count <= kVoiceTargetedMaxCommandGraphemes else {
            return nil
        }
        var chars = Array(normalized)

        // 礼貌前缀：仅绝对开头剥一层（操作数不可能位于整句开头，无歧义）。
        if let prefix = politePrefixes.first(where: { hasPrefix(chars, $0) }) {
            chars.removeFirst(prefix.count)
        }
        guard !chars.isEmpty else { return nil }
        // 疑问语气尾（吗/呢）：疑问句一律按普通文本处理（否定见 parseBody 把字句规则）。
        if let last = chars.last, last == "吗" || last == "呢" {
            return nil
        }

        // 语气尾：先剥一层再解析（与旧解析器口径一致）；剥后非指令再回退未剥重试——
        // 「删除刚才的吧」剥后落空，未剥 target="吧"。replacement 尾语气词同理被剥
        // （已知限制：「改成好吧」replacement 会得到「好」，列入文档）。
        if let last = chars.last, trailingParticles.contains(last) {
            var stripped = chars
            stripped.removeLast()
            if let result = parseBody(stripped) {
                return result
            }
        }
        if let result = parseBody(chars) {
            return result
        }
        return nil
    }

    /// 剥掉首尾空白/尾部标点并全角数字转半角；**保留操作数内部空白**。
    private static func normalize(_ text: String) -> String {
        var trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        while let last = trimmed.last, trailingPunctuation.contains(last) {
            trimmed.removeLast()
            trimmed = trimmed.trimmingCharacters(in: .whitespacesAndNewlines)
        }
        return fullWidthDigitsToHalfWidth(trimmed)
    }

    /// 语法主体：两种句式——
    ///   A. 动宾式：VERB SCOPE [POSITION] TARGET（整句消费完）；
    ///   B. 把字句：把 SCOPE [POSITION] TARGET (TRAILING_DELETE_VERB | REPLACE_SEPARATOR REST)。
    private static func parseBody(_ chars: [Character]) -> VoiceTargetedParseResult? {
        // B. 把字句。
        if hasPrefix(chars, "把") {
            var index = 1
            // 把字句内「把」与作用域之间不允许再夹字。
            guard consumeScope(chars, &index) else { return nil }
            let position = consumePosition(chars, &index)
            // 替换分隔符优先：只要句中存在分隔符即按替换解析——否则句中/句尾的
            // 删除动词会截断 target（「把刚才的删除按钮改成红色」/「把刚才的北京改成
            // 删除」两个最小复现）。删除分支仅在没有分隔符且动词恰在整句末尾时成立。
            if let separator = lastTokenIndex(chars, replaceSeparators) {
                let targetChars = Array(chars[index..<separator.index])
                let replacementChars = Array(chars[(separator.index + separator.token.count)...])
                // 「多个替换分隔符」/异常语法：target 或 replacement 内再含任何分隔符
                // 都判定为非法指令（吞掉并提示，不产生修改）。
                if containsAnyToken(targetChars, replaceSeparators) { return .invalid }
                if containsAnyToken(replacementChars, replaceSeparators) { return .invalid }
                let replacement = String(String(replacementChars)
                    .trimmingCharacters(in: .whitespacesAndNewlines))
                return buildCommand(
                    action: .replace(replacement), targetChars: targetChars, position: position)
            }
            if let deleteVerb = lastTokenIndex(chars, trailingDeleteVerbs) {
                let targetChars = Array(chars[index..<deleteVerb.index])
                guard deleteVerb.index + deleteVerb.token.count == chars.count else { return nil }
                // 否定副词（「把刚才的北京不要删掉」）：结构命中但语义为否定 → 吞掉提示。
                if containsNegation(targetChars) { return .invalid }
                return buildCommand(action: .delete, targetChars: targetChars, position: position)
            }
            return nil
        }
        // A. 动宾式。
        for verb in leadingDeleteVerbs {
            guard hasPrefix(chars, verb) else { continue }
            var index = verb.count
            guard consumeScope(chars, &index) else { continue }
            let position = consumePosition(chars, &index)
            let targetChars = Array(chars[index...])
            // 动宾式必须以作用域开头；「删除刚才的」本身是旧解析器的整段删除，
            // 这里 target 为空 → 让旧解析器优先处理（返回 nil，非 targeted 指令）。
            guard !targetChars.isEmpty else { return nil }
            // 否定副词（与 C++ 同语义）：结构命中但语义为否定 → 吞掉提示。
            if containsNegation(targetChars) { return .invalid }
            return buildCommand(action: .delete, targetChars: targetChars, position: position)
        }
        return nil
    }

    /// 由 target 字符（已定位）构建指令；空目标/超长 → nil（调用方决定 .invalid）。
    private static func buildCommand(
        action: VoiceTargetedAction, targetChars: [Character], position: VoiceTargetedPosition
    ) -> VoiceTargetedParseResult? {
        let target = String(String(targetChars).trimmingCharacters(in: .whitespacesAndNewlines))
        guard !target.isEmpty else { return .invalid }
        // P0 限单行：目标/替换含换行（\n 或 \r，防 CRLF 绕过）→ 非法指令。
        guard !target.contains("\n"), !target.contains("\r") else { return .invalid }
        guard target.count <= kVoiceTargetedMaxTargetGraphemes else { return .invalid }
        if case .replace(let replacement) = action {
            guard !replacement.isEmpty,
                  !replacement.contains("\n"),
                  !replacement.contains("\r"),
                  replacement.count <= kVoiceTargetedMaxReplacementGraphemes else {
                return .invalid
            }
        }
        return .command(VoiceTargetedCommand(action: action, target: target, position: position))
    }

    /// 否定副词检测（target 内出现「不要」或整词为「别」→ 视为否定式，拒绝执行）。
    private static func containsNegation(_ chars: [Character]) -> Bool {
        let target = String(chars).trimmingCharacters(in: .whitespacesAndNewlines)
        if target == "别" || target.hasPrefix("别") { return true }
        return target.contains("不要")
    }

    /// 在 index 处消费作用域标记；未命中返回 false（index 不动）。
    private static func consumeScope(_ chars: [Character], _ index: inout Int) -> Bool {
        for token in scopeTokens {
            if matchToken(chars, &index, token) { return true }
        }
        return false
    }

    /// 消费位置限定词（可选）；未命中返回 .unspecified（index 不动）。
    private static func consumePosition(_ chars: [Character], _ index: inout Int) -> VoiceTargetedPosition {
        if matchToken(chars, &index, positionFirst) { return .first }
        if matchToken(chars, &index, positionLast) { return .last }
        return .unspecified
    }

    private struct TokenHit {
        let index: Int
        let token: String
    }

    /// 在 chars 中找**最后一个**动词/分隔符 token 的出现位置。匹配成功即推进到
    /// token 之后（不额外跳 grapheme），保证紧邻的下一个 token 仍会被扫描到
    /// （「把刚才的删除删掉」的句尾动词必须成为 best）。
    private static func lastTokenIndex(_ chars: [Character], _ tokens: [String]) -> TokenHit? {
        var best: TokenHit?
        var index = 0
        while index < chars.count {
            var matched = false
            for token in tokens {
                var probe = index
                if matchToken(chars, &probe, token) {
                    best = TokenHit(index: index, token: token)
                    index = probe
                    matched = true
                    break
                }
            }
            if !matched {
                index += 1
            }
        }
        return best
    }

    private static func containsAnyToken(_ chars: [Character], _ tokens: [String]) -> Bool {
        for token in tokens {
            var index = 0
            while index < chars.count {
                if matchToken(chars, &index, token) { return true }
                index += 1
            }
        }
        return false
    }

    // ---- 词法工具 ----

    private static func hasPrefix(_ chars: [Character], _ token: String) -> Bool {
        let tokenChars = Array(token)
        guard chars.count >= tokenChars.count else { return false }
        for offset in 0..<tokenChars.count where chars[offset] != tokenChars[offset] { return false }
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

    private static func fullWidthDigitsToHalfWidth(_ text: String) -> String {
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

// ---- 匹配器 ----

/// 目标词在 remainingText 内的非重叠精确匹配（grapheme 口径；target 为空返回空表）。
public enum VoiceTargetedMatcher {
    public static func occurrences(of target: String, in remainingText: String) -> [VoiceTargetedOccurrence] {
        guard !target.isEmpty, !remainingText.isEmpty else { return [] }
        let haystack = Array(remainingText)
        let needle = Array(target)
        guard needle.count <= haystack.count else { return [] }

        // grapheme 边界 → UTF-16 偏移表：offsets[i] = 前 i 个 grapheme 的 UTF-16 长度。
        var offsets = [Int](repeating: 0, count: haystack.count + 1)
        for index in 0..<haystack.count {
            offsets[index + 1] = offsets[index] + String(haystack[index]).utf16.count
        }

        var result: [VoiceTargetedOccurrence] = []
        var searchFrom = 0
        while searchFrom + needle.count <= haystack.count {
            var matched = true
            for offset in 0..<needle.count where haystack[searchFrom + offset] != needle[offset] {
                matched = false
                break
            }
            if matched {
                let end = searchFrom + needle.count
                result.append(VoiceTargetedOccurrence(
                    index: result.count,
                    total: 0,  // 由 finish() 回填
                    graphemeRange: searchFrom..<end,
                    utf16Start: offsets[searchFrom],
                    utf16Length: offsets[end] - offsets[searchFrom]
                ))
                searchFrom = end  // 非重叠：前进过整个匹配
                continue
            }
            searchFrom += 1
        }
        if result.isEmpty { return [] }
        return result.map {
            VoiceTargetedOccurrence(
                index: $0.index, total: result.count, graphemeRange: $0.graphemeRange,
                utf16Start: $0.utf16Start, utf16Length: $0.utf16Length
            )
        }
    }
}

// ---- 计划器 ----

/// targeted 执行计划纯函数。安全模型（与旧尾部删除不同）：
///   * **任何 [voice_edit].verify_mode（strict/auto/off）下都必须 verified**——
///     严禁复用 8s unverified 盲删/盲改；读不到字段/光标一律拒绝；
///   * L1 要求**完整** remainingText 仍是字段后缀（允许末尾一个已记录 auto_enter
///     换行），不做旧「最长前缀」容错——用户手工变化会使目标偏移不可靠；
///   * 选区非空、光标不在字段末尾 → 拒绝。
public enum VoiceTargetedPlanner {
    public static func plan(
        command: VoiceTargetedCommand,
        remainingText: String,
        pressedEnter: Bool = false,
        fieldText: String? = nil,
        caretUTF16: Int? = nil,
        selectionLength: Int? = nil
    ) -> VoiceTargetedPlanResult {
        // 匹配与定位（纯逻辑；remainingText 为空时匹配为空 → notFound）。
        let matches = VoiceTargetedMatcher.occurrences(of: command.target, in: remainingText)
        guard !matches.isEmpty else { return .failure(.notFound) }
        let selected: VoiceTargetedOccurrence
        switch command.position {
        case .first:
            selected = matches[0]
        case .last:
            selected = matches[matches.count - 1]
        case .unspecified:
            guard matches.count == 1 else { return .failure(.ambiguous) }
            selected = matches[0]
        }

        // L1：targeted 恒 verified——任何 verify_mode 下都强制字段/光标校验。
        guard let fieldText, fieldText.utf16.count <= kVoiceEditMaxVerifiableFieldUTF16,
              let caretUTF16, let selectionLength else {
            return .failure(.verifyUnavailable)
        }
        if selectionLength > 0 { return .failure(.selectionPresent) }
        guard caretUTF16 == fieldText.utf16.count else { return .failure(.caretNotEnd) }
        // 完整 remainingText 后缀校验（允许末尾一个**已记录** auto_enter 换行；
        // targeted 不消费该换行，pressedEnter 保持原值）。不使用旧「最长前缀」容错。
        guard isFieldTailValid(fieldText: fieldText, remainingText: remainingText,
                               pressedEnter: pressedEnter) else {
            return .failure(.fieldMismatch)
        }

        // 执行后的 remainingText。
        let chars = Array(remainingText)
        let newRemaining: String
        switch command.action {
        case .delete:
            newRemaining = String(chars[0..<selected.graphemeRange.lowerBound])
                + String(chars[selected.graphemeRange.upperBound...])
        case .replace(let replacement):
            newRemaining = String(chars[0..<selected.graphemeRange.lowerBound])
                + replacement
                + String(chars[selected.graphemeRange.upperBound...])
        }
        return .plan(VoiceTargetedPlan(
            action: command.action,
            targetLength: command.target.count,
            occurrence: selected,
            newRemainingText: newRemaining
        ))
    }

    /// 字段尾部合法性：pressedEnter 时优先按「remaining+\n」判定（避免 remaining 自身
    /// 以换行结尾时误配裸后缀），否则按裸 remaining 判定——与协调器后缀拆解同一顺序。
    private static func isFieldTailValid(fieldText: String, remainingText: String,
                                         pressedEnter: Bool) -> Bool {
        if pressedEnter, fieldText.hasSuffix(remainingText + "\n") { return true }
        return fieldText.hasSuffix(remainingText)
    }
}
