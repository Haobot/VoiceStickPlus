import Foundation

// 语音编辑指令（Voice Edit Commands）纯逻辑层——解析器 + 删除计划。
// 对应 Doc/Plan/voice-edit-commands.md §5（语法表/数据结构）与 §6.3（目标校验算法
// 伪码/附录 B 失败码）。本文件位于 VoiceStickCore，不依赖 AppKit，供 App 与测试
// runner 共用；双端同构：Windows voice_edit_command.{h,cc} 使用同一张语法表与
// 同一份测试向量（延续 correction_pairs 的「双端同向量」约定）。
//
// 职责边界：
//   VoiceEditParser —— ASR final 文本（整句）→ 结构化指令；不做任何 IO/时间判断。
//   VoiceEditPlanner —— LastInjection 剩余文本 + 焦点字段/光标状态 + verify_mode
//                        → 退格次数 / 新剩余文本 / 新 pressedEnter，或拒绝码。
// L0 作用域闸（enabled / focused_app / 时间窗 / 前台应用 / AX 权限）由协调器
// 负责——那些条件依赖运行时状态，不进入纯函数。

/// 指令文本长度上限（方案 §5.3 误触发防护 #2）：超长文本不识别为指令。
public let kVoiceEditMaxCommandChars = 24

/// 焦点字段 UTF-16 长度上限（方案 §6.3）：超过即跳过 L1 校验（按不可验证处理），
/// 避免主线程读取/比对超大文本。
public let kVoiceEditMaxVerifiableFieldUTF16 = 2_000_000

// ---- 数据结构（方案 §5.5，两端同构）----

/// 指令动作：一期只支持「字 / 句 / 整段」三种动作（决策点 D3）。
public enum VoiceEditAction: Equatable {
    /// 删除最后 N 个字（grapheme 计数）。
    case deleteLastChars(Int)
    /// 删除最后 N 句（句子边界见哨兵集合）。
    case deleteLastSentences(Int)
    /// 删除本次注入剩余的整段文本。
    case deleteAll
}

/// 指令来源：显式数量 / 缺省数量（单独「删除」用 default_chars）/ 整段。
public enum VoiceEditCommandSource: Equatable {
    case explicitCount
    case defaultCount
    case all
}

/// 解析结果（方案 §5.5）。
public struct VoiceEditCommand: Equatable {
    public let action: VoiceEditAction
    public let source: VoiceEditCommandSource

    public init(action: VoiceEditAction, source: VoiceEditCommandSource) {
        self.action = action
        self.source = source
    }
}

/// L1 校验模式（方案 §6.5，配置 [voice_edit].verify_mode）。
public enum VoiceEditVerifyMode: String, CaseIterable, Equatable {
    /// 读不到字段/光标 → 一律拒绝（安全优先）。
    case strict
    /// 可验证→必须通过；不可验证→仅在 unverified 窗口内执行（默认）。
    case auto
    /// 不校验 L1，仅依赖 L0（高级/排障）。
    case off

    /// 配置值解析：未知值回落 auto（与方案默认一致）。
    public static func parse(_ raw: String) -> VoiceEditVerifyMode {
        VoiceEditVerifyMode(rawValue: raw.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()) ?? .auto
    }
}

/// 失败码（方案附录 B，raw = 日志用 code；用户提示文案由 App 层 L10nKey 映射）。
public enum VoiceEditFailureCode: String, CaseIterable, Equatable {
    case noInjection = "no_injection"
    case scopeExpired = "scope_expired"
    case appChanged = "app_changed"
    case fieldMismatch = "field_mismatch"
    case caretNotEnd = "caret_not_end"
    case selectionPresent = "selection_present"
    case verifyUnavailable = "verify_unavailable"
    case foregroundElevated = "foreground_elevated"
    case accessibilityDenied = "accessibility_denied"
    case injectFailed = "inject_failed"
}

/// 焦点字段状态（App 层 AX 读取 → 协调器 L1 校验 → planner）。
/// 方案 §6.3：kAXValueAttribute 文本 + kAXSelectedTextRange 选区范围一次读出；
/// 任一属性不可读（安全输入框/无文本焦点/无选区属性）时整体返回 nil = 不可验证。
/// 隐私红线：字段全文只在内存参与校验，不落库、不上传、不写日志内容。
public struct FocusedFieldState: Equatable {
    /// 焦点字段全文。
    public let text: String
    /// kAXSelectedTextRange.location（UTF-16 偏移；折叠光标 = 插入点）。
    public let caretUTF16: Int?
    /// kAXSelectedTextRange.length（0 = 折叠光标，>0 = 存在选区）。
    public let selectionLength: Int?

    public init(text: String, caretUTF16: Int?, selectionLength: Int?) {
        self.text = text
        self.caretUTF16 = caretUTF16
        self.selectionLength = selectionLength
    }
}

/// 本次删除是否通过了 L1 校验（日志/文案区分 verified / unverified）。
public enum VoiceEditVerification: String, Equatable {
    case verified
    case unverified
}

/// 删除计划（方案 §6.3 输出）：count = 退格次数（含尾随换行），执行后写回
/// LastInjection.remainingText / pressedEnter。
public struct VoiceEditDeletionPlan: Equatable {
    /// 退格注入次数（charCount + newlineCount）。
    public let count: Int
    /// 删除的 grapheme 数（不含换行）。
    public let charCount: Int
    /// 是否额外删掉 1 个尾随换行（auto_enter 注入的 Return）。
    public let newlineCount: Int
    /// 执行后的 LastInjection.remainingText。
    public let newRemainingText: String
    /// 执行后的 LastInjection.pressedEnter。
    public let newPressedEnter: Bool
    public let verification: VoiceEditVerification

    public init(count: Int, charCount: Int, newlineCount: Int,
                newRemainingText: String, newPressedEnter: Bool,
                verification: VoiceEditVerification) {
        self.count = count
        self.charCount = charCount
        self.newlineCount = newlineCount
        self.newRemainingText = newRemainingText
        self.newPressedEnter = newPressedEnter
        self.verification = verification
    }
}

/// 计划结果：成功计划或拒绝码（拒绝时绝不注入任何按键）。
public enum VoiceEditPlanResult: Equatable {
    case plan(VoiceEditDeletionPlan)
    case failure(VoiceEditFailureCode)
}

// ---- 解析器（方案 §5.1 语法表）----

/// 中文整句白名单解析器：只接受「整句恰好是一条编辑指令」，任何多余内容都不命中
/// （宁可当作普通文本上屏，也不误执行）。纯函数，无状态。
public enum VoiceEditParser {
    // 动词白名单（方案 §5.1）；多字动词按长度降序匹配。单独「删」只在带数量时接受。
    private static let verbs: [String] = [
        "删除", "删掉", "删去", "删了", "去掉", "移除", "清除", "取消", "删"
    ]
    /// 兜底动词（末尾动词句式）不含「删」以外的特殊限制；「删」同样要求显式数量。
    private static let positionWords: [String] = ["最后", "末尾", "结尾", "后面"]
    private static let demonstratives: [String] = ["这", "那"]
    private static let classifiers: [String] = ["个"]
    private static let charUnits: [String] = ["字符", "汉字", "文字", "字"]
    private static let sentenceUnits: [String] = ["句话", "句子", "句"]
    private static let politePrefixes: [String] = ["请帮我", "麻烦帮我", "帮我", "麻烦", "请"]
    private static let trailingParticles: Set<Character> = ["吧", "啊", "呀", "哦", "嘛"]
    private static let trailingPunctuation: Set<Character> = [
        "。", "．", ".", "！", "!", "？", "?", "，", ",", "；", ";", "、"
    ]
    /// 句末分隔符（方案 §6.3：。！？；与换行；另收 ASCII 变体与分号，句号不收回退）。
    private static let sentenceTerminators: Set<Character> = [
        "。", "！", "？", "；", "!", "?", ";", "\n", "\r"
    ]
    private static let chineseDigits: [Character: Int] = [
        "零": 0, "〇": 0, "一": 1, "二": 2, "两": 2, "俩": 2,
        "三": 3, "四": 4, "五": 5, "六": 6, "七": 7, "八": 8, "九": 9
    ]

    /// 单独动词 → 默认删除字数（决策点 D1：default_chars 默认 1）。
    private static let bareDeleteForms: Set<String> = [
        "删除", "删掉", "去掉", "删除一下", "删掉一下", "去掉一下"
    ]

    /// 整段删除白名单（「撤销/撤回」= 撤销本次输入；方案 §5.1 表格 + D1）。
    /// 全部为整句精确匹配，不参与其它语法组合。
    private static let deleteAllForms: Set<String> = [
        "删除刚才的", "删除刚才", "删掉刚才的", "删掉刚才",
        "删除刚才的内容", "删掉刚才的内容", "删除刚才说的", "删掉刚才说的",
        "删除刚才输入", "删除刚才输入的", "删掉刚才输入的",
        "删除刚输入的", "删掉刚输入的", "删除上次输入", "删掉上次输入",
        "删除本次输入", "删掉本次输入", "删除这次输入", "删掉这次输入",
        "删除全部", "删掉全部", "全部删除", "全部删掉", "都删掉", "都删除",
        "删除所有", "删掉所有", "清空", "清空刚才的", "撤销", "撤回",
        "撤销刚才的", "撤回刚才的",
        "把刚才的删掉", "把刚才删掉", "把刚才的删除",
        "把刚才输入的删掉", "把本次输入删掉", "把这次输入删掉"
    ]

    /// 整句解析：命中返回指令，否则 nil（调用方按原链路当普通文本处理）。
    /// - Parameters:
    ///   - text: ASR final 原文（可含尾部标点/首尾空白/全角数字）。
    ///   - defaultChars: 单独说「删除」时删几个字（配置 default_chars）。
    ///   - maxChars: 显式数量上限（配置 max_chars），越界一律不识别。
    public static func parse(_ text: String, defaultChars: Int = 1, maxChars: Int = 20) -> VoiceEditCommand? {
        let limit = max(1, maxChars)
        let defaultCount = min(max(1, defaultChars), limit)
        let normalized = normalize(text)
        guard !normalized.isEmpty, normalized.count <= kVoiceEditMaxCommandChars else { return nil }
        var chars = Array(normalized)

        // 礼貌前缀（「请/帮我/麻烦」，只剥一层，最长匹配优先）。
        if let prefix = politePrefixes.first(where: { hasPrefix(chars, $0) }) {
            chars.removeFirst(prefix.count)
        }
        // 语气尾（「吧/啊/呀/哦/嘛」，可叠加，循环剥离）。
        while let last = chars.last, trailingParticles.contains(last) {
            chars.removeLast()
        }
        guard !chars.isEmpty else { return nil }

        // 1) 单独「删除/删掉/去掉」→ default_chars（D1：默认 1）。
        let bare = String(chars)
        if bareDeleteForms.contains(bare) {
            return VoiceEditCommand(action: .deleteLastChars(defaultCount), source: .defaultCount)
        }
        // 2) 整段删除白名单（撤销/撤回/清空/删除本次输入…）。
        if deleteAllForms.contains(bare) {
            return VoiceEditCommand(action: .deleteAll, source: .all)
        }
        // 3) 动宾式：删除最后两个字 / 删除两个字 / 删掉最后一句 / 删除最后这句话。
        if let command = matchLeadingVerb(chars, limit: limit) { return command }
        // 4) 把字句/后置动词式：把最后两个字删掉 / 最后两个字删掉 / 把最后一句删掉。
        if let command = matchTrailingVerb(chars, limit: limit) { return command }
        return nil
    }

    /// 句尾最后 N 句的 grapheme 数（供计划算法使用；无标点时整段算一句）。
    /// 每个句子 = 内容 + 连续句末分隔符；末尾无分隔符的残段也算一句。
    static func trailingSentenceGraphemeCount(_ text: String, sentences: Int) -> Int {
        guard sentences > 0 else { return 0 }
        let chars = Array(text)
        guard !chars.isEmpty else { return 0 }
        var boundaries: [Int] = []   // 每句结束（不含）索引
        var index = 0
        while index < chars.count {
            if sentenceTerminators.contains(chars[index]) {
                var end = index + 1
                while end < chars.count && sentenceTerminators.contains(chars[end]) { end += 1 }
                boundaries.append(end)
                index = end
            } else {
                index += 1
            }
        }
        let lastBoundary = boundaries.last ?? 0
        if lastBoundary < chars.count { boundaries.append(chars.count) }
        guard !boundaries.isEmpty else { return 0 }
        let take = min(sentences, boundaries.count)
        let firstKept = boundaries.count - take
        let start = firstKept == 0 ? 0 : boundaries[firstKept - 1]
        return chars.count - start
    }

    // ---- 语法组合 ----

    /// 动宾式：VERB [POS] [DEMON] [QTY] [个] UNIT（整句消费完）。
    private static func matchLeadingVerb(_ chars: [Character], limit: Int) -> VoiceEditCommand? {
        for verb in verbs {
            var index = 0
            guard matchToken(chars, &index, verb) else { continue }
            guard let object = parseObject(chars, &index, limit: limit) else { continue }
            guard index == chars.count else { continue }
            // 单独「删」只在带数量时接受（方案 §5.1）。
            if verb == "删" && !object.hadQuantity { continue }
            return VoiceEditCommand(action: object.action, source: object.source)
        }
        return nil
    }

    /// 后置动词式：[把] [POS] [DEMON] [QTY] [个] UNIT VERB（整句消费完）。
    private static func matchTrailingVerb(_ chars: [Character], limit: Int) -> VoiceEditCommand? {
        var index = 0
        _ = matchToken(chars, &index, "把")
        guard let object = parseObject(chars, &index, limit: limit) else { return nil }
        guard let verb = matchAnyVerb(chars, &index) else { return nil }
        guard index == chars.count else { return nil }
        if verb == "删" && !object.hadQuantity { return nil }
        return VoiceEditCommand(action: object.action, source: object.source)
    }

    private struct ObjectMatch {
        let action: VoiceEditAction
        let source: VoiceEditCommandSource
        let hadQuantity: Bool
    }

    /// 解析 [POS] [DEMON] [QTY] [个] UNIT；数量对「字」类单位必填、对「句」类可缺省 1。
    private static func parseObject(_ chars: [Character], _ index: inout Int, limit: Int) -> ObjectMatch? {
        _ = matchFirstToken(chars, &index, positionWords)
        _ = matchFirstToken(chars, &index, demonstratives)
        let quantity = parseNumber(chars, &index)
        _ = matchFirstToken(chars, &index, classifiers)
        let hadQuantity = quantity != nil

        if matchFirstToken(chars, &index, charUnits) != nil {
            guard let quantity, (1...limit).contains(quantity) else { return nil }
            return ObjectMatch(action: .deleteLastChars(quantity), source: .explicitCount,
                               hadQuantity: true)
        }
        if matchFirstToken(chars, &index, sentenceUnits) != nil {
            let count = quantity ?? 1
            guard (1...limit).contains(count) else { return nil }
            return ObjectMatch(action: .deleteLastSentences(count),
                               source: hadQuantity ? .explicitCount : .defaultCount,
                               hadQuantity: hadQuantity)
        }
        return nil
    }

    /// 匹配任一动词（末尾式使用；长度降序，整句消费由调用方保证）。
    private static func matchAnyVerb(_ chars: [Character], _ index: inout Int) -> String? {
        for verb in verbs {
            var probe = index
            if matchToken(chars, &probe, verb) {
                index = probe
                return verb
            }
        }
        return nil
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

    /// 依次尝试一组 token，命中即消费并返回（最长匹配由调用方列表顺序保证）。
    private static func matchFirstToken(
        _ chars: [Character], _ index: inout Int, _ tokens: [String]
    ) -> String? {
        for token in tokens {
            var probe = index
            if matchToken(chars, &probe, token) {
                index = probe
                return token
            }
        }
        return nil
    }

    /// 数量解析：阿拉伯数字（连续）或中文数字（一~九十九），返回 nil 表示不是数量。
    private static func parseNumber(_ chars: [Character], _ index: inout Int) -> Int? {
        guard index < chars.count else { return nil }
        // 阿拉伯数字：全角已在 normalize 中转半角。
        if chars[index].isASCII, let digit = chars[index].wholeNumberValue, (0...9).contains(digit) {
            var value = 0
            var count = 0
            while index < chars.count, chars[index].isASCII,
                  let next = chars[index].wholeNumberValue, (0...9).contains(next) {
                value = value * 10 + next
                index += 1
                count += 1
                if value > 999 { return nil }   // 长数字串不识别，防误判
            }
            return count > 0 ? value : nil
        }
        // 中文数字：十 / 十X / X十 / X十Y。
        if chars[index] == "十" {
            index += 1
            var value = 10
            if index < chars.count, let unit = chineseDigits[chars[index]], (1...9).contains(unit) {
                value += unit
                index += 1
            }
            return value
        }
        if let digit = chineseDigits[chars[index]], (1...9).contains(digit) {
            index += 1
            if index < chars.count, chars[index] == "十" {
                index += 1
                var value = digit * 10
                if index < chars.count, let unit = chineseDigits[chars[index]], (1...9).contains(unit) {
                    value += unit
                    index += 1
                }
                return value
            }
            return digit
        }
        return nil
    }

    // ---- 归一化（方案 §5.1：首尾空白、尾部标点、全角数字、内部空白）----

    private static func normalize(_ text: String) -> String {
        var trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        // 尾部标点（可叠加）去掉后再 trim，允许「删掉最后两个字。」「删除，」。
        while let last = trimmed.last, trailingPunctuation.contains(last) {
            trimmed.removeLast()
            trimmed = trimmed.trimmingCharacters(in: .whitespacesAndNewlines)
        }
        let halfWidthDigits = fullWidthDigitsToHalfWidth(trimmed)
        // ASR 可能在中英/数字之间插空格（「最后 2 个字」）：匹配口径忽略内部空白。
        return String(halfWidthDigits.filter { !$0.isWhitespace })
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

// ---- 删除计划（方案 §6.3 伪码）----

/// 删除计划纯函数：把「剩余注入文本 + 焦点字段/光标状态 + verify_mode」换算成退格
/// 次数与写回记录，或拒绝码。绝不注入、绝不 IO——执行与收尾在协调器。
public enum VoiceEditPlanner {
    /// 生成删除计划。
    /// - Parameters:
    ///   - remainingText: LastInjection.remainingText（理论上仍在输入框的剩余文本）。
    ///   - pressedEnter: LastInjection.pressedEnter（注入时 auto_enter 的实际值）。
    ///   - action: 解析出的动作。
    ///   - fieldText: AX 读到的焦点字段全文；nil = 不可验证。
    ///   - caretUTF16: kAXSelectedTextRange.location（UTF-16 偏移）；nil = 不可验证。
    ///   - selectionLength: kAXSelectedTextRange.length；>0 表示有选区。
    ///   - maxChars: 配置 max_chars；作为「字」动作的纵深防御上限（句/整段不受字数上限，
    ///     整段删除按方案 §6.4 属例外；sentence 的单位是句不是字）。
    ///   - verifyMode: 配置 verify_mode。
    ///   - unverifiedWindowOpen: auto 模式下 now-injectedAt <= unverified_seconds。
    public static func plan(
        remainingText: String,
        pressedEnter: Bool,
        action: VoiceEditAction,
        fieldText: String? = nil,
        caretUTF16: Int? = nil,
        selectionLength: Int? = nil,
        maxChars: Int = 20,
        verifyMode: VoiceEditVerifyMode = .auto,
        unverifiedWindowOpen: Bool = false
    ) -> VoiceEditPlanResult {
        let remaining = Array(remainingText)
        guard !remaining.isEmpty else { return .failure(.noInjection) }

        if verifyMode == .off {
            return unverifiedPlan(remaining: remaining, pressedEnter: pressedEnter, action: action, maxChars: maxChars)
        }

        // 大文本保护（方案 §6.3）：字段过长按不可验证处理，避免主线程比对开销。
        let field: String? = {
            guard let fieldText, fieldText.utf16.count <= kVoiceEditMaxVerifiableFieldUTF16 else {
                return nil
            }
            return fieldText
        }()
        guard let field, let caret = caretUTF16, let selection = selectionLength else {
            switch verifyMode {
            case .strict:
                return .failure(.verifyUnavailable)
            case .auto:
                guard unverifiedWindowOpen else { return .failure(.verifyUnavailable) }
                return unverifiedPlan(remaining: remaining, pressedEnter: pressedEnter, action: action, maxChars: maxChars)
            case .off:
                return unverifiedPlan(remaining: remaining, pressedEnter: pressedEnter, action: action, maxChars: maxChars)
            }
        }

        // 选区非空先拒绝（方案 §6.4：避免「删 N 字」变成删除选区）。
        if selection > 0 { return .failure(.selectionPresent) }
        // 光标必须折叠且位于字段末尾（UTF-16 偏移口径）。
        guard caret == field.utf16.count else { return .failure(.caretNotEnd) }

        // auto_enter 的换行不假设「一定有」：字段实际以换行结尾时，前缀匹配在
        // 「去掉一个尾随换行」的字段内容上做（方案 §6.3 示例的隐含前提——
        // 字段「你好世界\n」/ remaining「你好世界」必须得到 eff=4、newline=1）。
        let newlinePresent = pressedEnter && field.hasSuffix("\n")
        let contentField = newlinePresent ? String(field.dropLast()) : field
        // 允许用户手动删过尾部：取 remainingText 的最长前缀，使其是字段内容的后缀。
        let minLength = min(remaining.count, contentField.count)
        var effectiveLength = 0
        if minLength >= 1 {
            for length in stride(from: minLength, through: 1, by: -1) {
                if contentField.hasSuffix(String(remaining[0..<length])) {
                    effectiveLength = length
                    break
                }
            }
        }
        guard effectiveLength > 0 else { return .failure(.fieldMismatch) }
        let effectiveChars = Array(remaining[0..<effectiveLength])
        let effectiveText = String(effectiveChars)

        // eff>0 已保证字段内容以 effectiveText 结尾；尾随换行存在即需多删 1 次。
        let newlineCount = newlinePresent ? 1 : 0
        let charCap = max(1, maxChars)
        let targetChars: Int
        switch action {
        case .deleteLastChars(let count):
            // 纵深防御：解析器已按 max_chars 拒绝越界指令，planner 再兜一层——
            // 任何调用方都不可能在单条字指令里删超过配置上限的字数。
            targetChars = min(max(0, count), charCap)
        case .deleteLastSentences(let count):
            targetChars = VoiceEditParser.trailingSentenceGraphemeCount(
                effectiveText, sentences: max(0, count))
        case .deleteAll:
            targetChars = effectiveChars.count
        }
        // 不越过注入边界：N 永远 clamp 到 effectiveText 的 grapheme 数。
        let deleteChars = min(targetChars, effectiveChars.count)
        guard deleteChars > 0, deleteChars + newlineCount > 0 else {
            return .failure(.fieldMismatch)
        }
        // L1 已验证字段末尾：字段里没有尾随换行（或已本次删掉），记录一律置 false，
        // 防止之后降级到 unverified 时重复多发退格（比方案伪码更保守）。
        return .plan(VoiceEditDeletionPlan(
            count: deleteChars + newlineCount,
            charCount: deleteChars,
            newlineCount: newlineCount,
            newRemainingText: String(effectiveChars.dropLast(deleteChars)),
            newPressedEnter: false,
            verification: .verified
        ))
    }

    /// 不可验证分支（strict 拒绝、auto 窗口内允许、off 直接走）：按 remainingText
    /// 估算，pressedEnter 时多发 1 次退格并在执行后置 false（方案 §6.3/§7.3）。
    private static func unverifiedPlan(
        remaining: [Character], pressedEnter: Bool, action: VoiceEditAction, maxChars: Int
    ) -> VoiceEditPlanResult {
        let charCap = max(1, maxChars)
        let targetChars: Int
        switch action {
        case .deleteLastChars(let count):
            targetChars = min(max(0, count), charCap)
        case .deleteLastSentences(let count):
            targetChars = VoiceEditParser.trailingSentenceGraphemeCount(
                String(remaining), sentences: max(0, count))
        case .deleteAll:
            targetChars = remaining.count
        }
        let deleteChars = min(targetChars, remaining.count)
        guard deleteChars > 0 else { return .failure(.noInjection) }
        let newlineCount = pressedEnter ? 1 : 0
        return .plan(VoiceEditDeletionPlan(
            count: deleteChars + newlineCount,
            charCount: deleteChars,
            newlineCount: newlineCount,
            newRemainingText: String(remaining.dropLast(deleteChars)),
            newPressedEnter: false,
            verification: .unverified
        ))
    }
}