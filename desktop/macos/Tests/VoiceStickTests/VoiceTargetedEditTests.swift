import Foundation
import VoiceStickCore

// Targeted 语音编辑（删除/替换最近一次输入中的指定词）解析器 + 匹配器 + 计划器单测。
// 与 Windows tests/core_tests_voice_edit.cc 的 targeted 段使用同一张向量表
//（双端同向量约定）。运行：cd desktop/macos && swift run VoiceStickTests。

// ---- 断言辅助 ----

private func expectTargeted(
    _ text: String, _ expected: VoiceTargetedCommand, _ name: String
) {
    guard let result = VoiceTargetedParser.parse(text) else {
        check(false, "\(name)（期望 targeted 指令，实际非指令）")
        return
    }
    guard case .command(let command) = result else {
        check(false, "\(name)（期望指令，实际 invalid）")
        return
    }
    checkEqual(command, expected, name)
}

private func expectTargetedInvalid(_ text: String, _ name: String) {
    guard let result = VoiceTargetedParser.parse(text) else {
        check(false, "\(name)（期望 invalid，实际非指令）")
        return
    }
    guard case .invalid = result else {
        check(false, "\(name)（期望 invalid，实际命中指令）")
        return
    }
    check(true, name)
}

private func expectNoTargeted(_ text: String, _ name: String) {
    checkNil(VoiceTargetedParser.parse(text), "\(name)（应为非指令）")
}

private func deleteCommand(_ target: String, _ position: VoiceTargetedPosition = .unspecified,
                           _ hasExplicitScope: Bool = true) -> VoiceTargetedCommand {
    VoiceTargetedCommand(action: .delete, target: target, position: position,
                         hasExplicitScope: hasExplicitScope)
}

private func replaceCommand(_ target: String, _ replacement: String,
                            _ position: VoiceTargetedPosition = .unspecified,
                            _ hasExplicitScope: Bool = true) -> VoiceTargetedCommand {
    VoiceTargetedCommand(action: .replace(replacement), target: target, position: position,
                         hasExplicitScope: hasExplicitScope)
}

// ---- 解析器正例（首版全部句式）----

private func runTargetedParserPositiveTests() {
    // 动宾式删除（动词 × 作用Scope × 位置 × 目标）。
    expectTargeted("删除刚才的北京", deleteCommand("北京"), "正例：删除刚才的北京")
    expectTargeted("删掉刚才的北京", deleteCommand("北京"), "正例：删掉刚才的北京")
    expectTargeted("删去刚才的北京", deleteCommand("北京"), "正例：删去刚才的北京")
    expectTargeted("去掉刚才的北京", deleteCommand("北京"), "正例：去掉刚才的北京")
    expectTargeted("删除刚刚的北京", deleteCommand("北京"), "正例：刚刚的作用域")
    expectTargeted("删除刚才北京", deleteCommand("北京"), "正例：无「的」作用域")

    // 位置限定（第一个/最后一个；两种语序）。
    expectTargeted("删除刚才第一个北京", deleteCommand("北京", .first), "正例：删除刚才第一个")
    expectTargeted("删除刚才最后一个北京", deleteCommand("北京", .last), "正例：删除刚才最后一个")
    expectTargeted("把刚才第一个北京删掉", deleteCommand("北京", .first), "正例：把字句第一个")
    expectTargeted("把刚才最后一个北京删掉", deleteCommand("北京", .last), "正例：把字句最后一个")
    expectTargeted("把刚才的最后一个北京删掉", deleteCommand("北京", .last),
                   "正例：刚才的最后一个语序")

    // 把字句删除（同义删除动词）。
    expectTargeted("把刚才的北京删掉", deleteCommand("北京"), "正例：把字句删掉")
    expectTargeted("把刚才的北京删除", deleteCommand("北京"), "正例：把字句删除")
    expectTargeted("把刚才的北京去掉", deleteCommand("北京"), "正例：把字句去掉")
    expectTargeted("把刚才的北京删去", deleteCommand("北京"), "正例：把字句删去")
    expectTargeted("请把刚才的北京删除", deleteCommand("北京"), "正例：句尾删除动词")

    // 分隔符优先：句中/句尾出现删除动词仍按替换解析（最小复现回归）。
    expectTargeted("把刚才的删除按钮改成红色", replaceCommand("删除按钮", "红色"),
                   "正例：目标含删除动词的替换")
    expectTargeted("把刚才的北京改成删除", replaceCommand("北京", "删除"),
                   "正例：替换值为删除动词")
    // 目标本身含删除动词、句尾动词的纯删除。
    expectTargeted("把刚才的删除删掉", deleteCommand("删除"), "正例：目标=删除")
    expectTargeted("把刚才的删掉删除", deleteCommand("删掉"), "正例：目标=删掉")

    // 把字句替换（5 个分隔符）。
    expectTargeted("把刚才的北京改成上海", replaceCommand("北京", "上海"), "正例：改成")
    expectTargeted("把刚才的北京改为上海", replaceCommand("北京", "上海"), "正例：改为")
    expectTargeted("把刚才的北京换成上海", replaceCommand("北京", "上海"), "正例：换成")
    expectTargeted("把刚才的北京替换为上海", replaceCommand("北京", "上海"), "正例：替换为")
    expectTargeted("把刚才的北京替换成上海", replaceCommand("北京", "上海"), "正例：替换成")
    expectTargeted("把刚才最后一个北京改成上海", replaceCommand("北京", "上海", .last),
                   "正例：最后一个+改成")
    expectTargeted("把刚才的最后一个北京改成上海", replaceCommand("北京", "上海", .last),
                   "正例：的最后一个+改成")

    // 多字/短语目标（P0「词/短语」按连续文本片段精确匹配）。
    expectTargeted("删除刚才的北京大学", deleteCommand("北京大学"), "正例：多字目标")
    expectTargeted("把刚才的北京大学改成清华大学", replaceCommand("北京大学", "清华大学"),
                   "正例：多字目标+多字替换")
    expectTargeted("把刚才的new york改成上海", replaceCommand("new york", "上海"),
                   "正例：操作数内部空白保留")

    // 归一化：尾部标点/首尾空白/礼貌前缀/语气尾。
    expectTargeted("删除刚才的北京。", deleteCommand("北京"), "正例：尾部句号")
    expectTargeted("  把刚才的北京改成上海。 ", replaceCommand("北京", "上海"),
                   "正例：首尾空白+句号")
    expectTargeted("请把刚才的北京删掉", deleteCommand("北京"), "正例：请前缀")
    expectTargeted("请帮我删除刚才的北京", deleteCommand("北京"), "正例：请帮我前缀")
    expectTargeted("麻烦把刚才的北京改成上海", replaceCommand("北京", "上海"), "正例：麻烦前缀")
    expectTargeted("把刚才的北京删掉吧", deleteCommand("北京"), "正例：吧语气尾")
    expectTargeted("把刚才的北京改成上海吧", replaceCommand("北京", "上海"), "正例：替换+吧语气尾")
    expectTargeted("删除刚才的北京啊", deleteCommand("北京"), "正例：啊语气尾")

    // 目标以语气词结尾：贪心优先保住操作数（不剥）。
    expectTargeted("删除刚才的吧", deleteCommand("吧"), "正例：target=吧 不剥语气尾")

    // 长度边界：目标 32 / 替换 64 graphemes。
    let target32 = String(repeating: "字", count: 32)
    let target33 = String(repeating: "字", count: 33)
    let replacement64 = String(repeating: "替", count: 64)
    let replacement65 = String(repeating: "替", count: 65)
    expectTargeted("删除刚才的\(target32)", deleteCommand(target32), "正例：目标 32 边界")
    expectTargeted("把刚才的\(target32)改成\(replacement64)",
                   replaceCommand(target32, replacement64), "正例：替换 64 边界")
    expectTargetedInvalid("删除刚才的\(target33)", "非法：目标 33 超限")
    expectTargetedInvalid("把刚才的\(target32)改成\(replacement65)", "非法：替换 65 超限")
}

// ---- 解析器负例与非法指令 ----

private func runTargetedParserNegativeTests() {
    // 自然简写：解析为 hasExplicitScope=false 的候选，协调器仅在目标存在时接管。
    expectTargeted("把北京改成上海", replaceCommand("北京", "上海", .unspecified, false),
                   "正例：自然简写替换")
    expectTargeted("删除北京", deleteCommand("北京", .unspecified, false),
                   "正例：自然简写删除")
    expectTargeted("把北京删掉", deleteCommand("北京", .unspecified, false),
                   "正例：自然简写把字句删除")
    expectTargeted("把第一个北京改成上海", replaceCommand("北京", "上海", .first, false),
                   "正例：自然简写带位置")
    expectTargeted("请把删除按钮改成红色", replaceCommand("删除按钮", "红色", .unspecified, false),
                   "正例：礼貌前缀自然简写")
    // 简写结构异常时无法可靠判断意图，回退普通文本而非吞掉。
    expectNoTargeted("把改成上海", "负例：简写空目标")
    expectNoTargeted("把北京改成", "负例：简写空替换")
    expectNoTargeted("把北京改成上海改成广州", "负例：简写多分隔符")

    // 旧解析器在协调器优先。「删除最后两个字」可形成简写候选，但不会抢占旧语义。
    expectNoTargeted("删除刚才的", "负例：删除刚才的=旧整段删除")
    expectTargeted("删除最后两个字", deleteCommand("最后两个字", .unspecified, false),
                   "正例：旧字删除同时是简写候选（协调器旧解析优先）")
    expectNoTargeted("删除", "负例：单独删除")

    // 混合正文/包含式/否定/疑问：按普通文本上屏或判非法指令（吞掉提示，绝不修改）。
    expectNoTargeted("你好删除刚才的北京", "负例：正文+指令混说")
    expectNoTargeted("把刚才的文件删除一下可以吗", "负例：疑问+夹带")
    expectNoTargeted("不删除刚才的北京", "负例：否定（前导）")
    expectTargetedInvalid("把刚才的北京不要删掉", "非法：否定把字句")
    expectTargetedInvalid("把刚才的别删掉", "非法：别删掉")
    expectNoTargeted("把刚才的北京删掉吗", "负例：疑问语气尾（吗不识别）")
    expectNoTargeted("删除刚才的北京好吗", "负例：疑问语气尾（好吗）")

    // 结构命中但操作数非法 → invalid（吞掉并提示，不上屏、不修改）。
    expectTargetedInvalid("把刚才的改成上海", "非法：空目标")
    expectTargetedInvalid("把刚才的改成", "非法：空目标且无替换")
    expectTargetedInvalid("把刚才的北京改成", "非法：空替换")
    expectTargetedInvalid("把刚才的 改成上海", "非法：空白空目标")
    expectTargetedInvalid("把刚才的北京改成上海改成广州", "非法：多替换分隔符")
    expectTargetedInvalid("把刚才的改成改成上海", "非法：目标含分隔符")
    expectTargetedInvalid("把刚才的第一个改成上海", "非法：位置限定后无目标")
    expectTargetedInvalid("把刚才的北京改成改成", "非法：空替换且多分隔符")
    expectTargetedInvalid("删除刚才的北\n京", "非法：多行目标")
    expectTargetedInvalid("把刚才的北京改成上\n海", "非法：多行替换")
    expectTargetedInvalid("删除刚才的北\r京", "非法：CR 目标（单行约束）")
    expectTargetedInvalid("把刚才的北京改成上\r海", "非法：CR 替换（单行约束）")

    // 动宾式否定（与 C++ 同语义）。
    expectTargetedInvalid("删除刚才的别删", "非法：动宾否定别")
    expectTargetedInvalid("删除刚才的北京不要", "非法：动宾否定不要")

    // 异常语法。
    expectNoTargeted("", "负例：空串")
    expectNoTargeted("把", "负例：单字")
    expectNoTargeted("把刚才的", "负例：作用域后无内容")
    expectNoTargeted("把刚才的北京", "负例：把字句无动词/分隔符")
    expectNoTargeted("把刚才的北京删掉快点", "负例：动词后夹带正文")
    // replacement 贪婪到句尾（整句语法的确定性解析；混说正文会成为操作数，由
    // 匹配/校验兜底，P1 由语义编辑解决）。
    expectTargeted("把刚才的北京改成上海然后继续", replaceCommand("北京", "上海然后继续"),
                   "正例：替换操作数取剩余全文")

    // 超长指令（超过 128 graphemes）。
    let huge = "删除刚才的" + String(repeating: "长", count: 130)
    expectNoTargeted(huge, "负例：超长指令")
}

// ---- 匹配器向量 ----

private func runTargetedMatcherTests() {
    // CJK：唯一匹配 + UTF-16 偏移（CJK 1 字 = 1 UTF-16）。
    let cjk = VoiceTargetedMatcher.occurrences(of: "北京", in: "我去北京上班")
    checkEqual(cjk.count, 1, "匹配：CJK 唯一")
    checkEqual(cjk[0].graphemeRange, 2..<4, "匹配：CJK grapheme 区间")
    checkEqual(cjk[0].utf16Start, 2, "匹配：CJK utf16Start")
    checkEqual(cjk[0].utf16Length, 2, "匹配：CJK utf16Length")

    // ASCII。
    let ascii = VoiceTargetedMatcher.occurrences(of: "ab", in: "xabcab")
    checkEqual(ascii.count, 2, "匹配：ASCII 两处")
    checkEqual(ascii[0].graphemeRange, 1..<3, "匹配：ASCII 第一处区间")
    checkEqual(ascii[1].graphemeRange, 4..<6, "匹配：ASCII 第二处区间")
    checkEqual(ascii[1].utf16Start, 4, "匹配：ASCII utf16Start")

    // 非重叠：目标自重复（"aa" in "aaaa" → 0、2 两处，不是 3 处）。
    let overlap = VoiceTargetedMatcher.occurrences(of: "aa", in: "aaaa")
    checkEqual(overlap.count, 2, "匹配：非重叠 2 处")
    checkEqual(overlap[0].graphemeRange, 0..<2, "匹配：非重叠第一处")
    checkEqual(overlap[1].graphemeRange, 2..<4, "匹配：非重叠第二处")

    // emoji（surrogate pair = 2 UTF-16，1 grapheme）。
    let emoji = VoiceTargetedMatcher.occurrences(of: "🙂", in: "好🙂吗🙂")
    checkEqual(emoji.count, 2, "匹配：emoji 两处")
    checkEqual(emoji[0].graphemeRange, 1..<2, "匹配：emoji grapheme 区间")
    checkEqual(emoji[0].utf16Start, 1, "匹配：emoji utf16Start（CJK 后 1 码元）")
    checkEqual(emoji[0].utf16Length, 2, "匹配：emoji utf16Length=2")
    checkEqual(emoji[1].utf16Start, 4, "匹配：第二处 emoji utf16Start")

    // 组合字符（e + U+0301 = 2 UTF-16，1 grapheme）。
    let combining = VoiceTargetedMatcher.occurrences(of: "e\u{301}", in: "cafe\u{301}x")
    checkEqual(combining.count, 1, "匹配：组合字符唯一")
    checkEqual(combining[0].utf16Start, 3, "匹配：组合字符 utf16Start")
    checkEqual(combining[0].utf16Length, 2, "匹配：组合字符 utf16Length=2")

    // 开头/中间/末尾。
    checkEqual(VoiceTargetedMatcher.occurrences(of: "你", in: "你好").first?.graphemeRange,
               0..<1, "匹配：目标在开头")
    checkEqual(VoiceTargetedMatcher.occurrences(of: "你", in: "好你").first?.graphemeRange,
               1..<2, "匹配：目标在末尾")
    checkNil(VoiceTargetedMatcher.occurrences(of: "你", in: "他好").first,
             "匹配：无匹配返回空")
    checkEqual(VoiceTargetedMatcher.occurrences(of: "你", in: "").count, 0,
               "匹配：空 remaining 返回空")
    checkEqual(VoiceTargetedMatcher.occurrences(of: "", in: "你好").count, 0,
               "匹配：空目标返回空")
}

// ---- 计划器向量 ----

private func runTargetedPlannerTests() {
    let fieldState = FocusedFieldState(text: "我去北京上班", caretUTF16: 6, selectionLength: 0)

    // 唯一匹配：删除（字段无换行）。
    guard case .plan(let deletePlan) = VoiceTargetedPlanner.plan(
        command: deleteCommand("北京"), remainingText: "我去北京上班",
        fieldText: fieldState.text, caretUTF16: fieldState.caretUTF16,
        selectionLength: fieldState.selectionLength) else {
        check(false, "计划：唯一删除应成功")
        return
    }
    checkEqual(deletePlan.newRemainingText, "我去上班", "计划：唯一删除 newRemaining")
    checkEqual(deletePlan.targetLength, 2, "计划：目标长度")
    checkEqual(deletePlan.occurrence.utf16Start, 2, "计划：删除选区 utf16Start")
    checkEqual(deletePlan.occurrence.utf16Length, 2, "计划：删除选区 utf16Length")

    // 唯一匹配：替换。
    guard case .plan(let replacePlan) = VoiceTargetedPlanner.plan(
        command: replaceCommand("北京", "上海"), remainingText: "我去北京上班",
        fieldText: fieldState.text, caretUTF16: fieldState.caretUTF16,
        selectionLength: fieldState.selectionLength) else {
        check(false, "计划：唯一替换应成功")
        return
    }
    checkEqual(replacePlan.newRemainingText, "我去上海上班", "计划：唯一替换 newRemaining")

    // auto_enter 换行：字段 = remaining + "\n"，仍通过后缀校验（targeted 不动换行）。
    guard case .plan(let newlinePlan) = VoiceTargetedPlanner.plan(
        command: deleteCommand("北京"), remainingText: "我去北京上班", pressedEnter: true,
        fieldText: "我去北京上班\n", caretUTF16: 7, selectionLength: 0) else {
        check(false, "计划：auto_enter 换行应通过")
        return
    }
    checkEqual(newlinePlan.newRemainingText, "我去上班", "计划：换行不影响 newRemaining")

    // 未记录换行（pressedEnter=false 但字段多一个换行）→ fieldMismatch。
    guard case .failure(let newlineFail) = VoiceTargetedPlanner.plan(
        command: deleteCommand("北京"), remainingText: "我去北京上班", pressedEnter: false,
        fieldText: "我去北京上班\n", caretUTF16: 7, selectionLength: 0) else {
        check(false, "计划：未记录换行应拒绝")
        return
    }
    checkEqual(newlineFail, .fieldMismatch, "计划：未记录换行 fieldMismatch")

    // 前缀 + remaining（用户既有内容在前）：通过后缀校验。
    guard case .plan(let prefixPlan) = VoiceTargetedPlanner.plan(
        command: deleteCommand("北京"), remainingText: "我去北京上班",
        fieldText: "前面的话我去北京上班", caretUTF16: 10, selectionLength: 0) else {
        check(false, "计划：字段前缀应通过")
        return
    }
    checkEqual(prefixPlan.newRemainingText, "我去上班", "计划：字段前缀 newRemaining")
    checkEqual(prefixPlan.occurrence.utf16Start, 2,
               "计划：utf16 偏移相对 remainingText（与字段前缀无关）")

    // 手工改变 remaining（旧最长前缀容错**不得**生效）→ fieldMismatch。
    expectTargetedPlanFailure(
        command: deleteCommand("北京"), remainingText: "我去北京上班",
        fieldText: "我去北上班", caretUTF16: 5, selectionLength: 0,
        .fieldMismatch, "计划：手工删字 → fieldMismatch（不用前缀容错）")
    expectTargetedPlanFailure(
        command: deleteCommand("北京"), remainingText: "我去北京上班",
        fieldText: "我去北京上班吗", caretUTF16: 7, selectionLength: 0,
        .fieldMismatch, "计划：追加手打 → fieldMismatch")

    // 多处匹配。
    expectTargetedPlanFailure(
        command: deleteCommand("北京"), remainingText: "北京和北京",
        fieldText: "北京和北京", caretUTF16: 5, selectionLength: 0,
        .ambiguous, "计划：多处未指定位置 → ambiguous")
    guard case .plan(let firstPlan) = VoiceTargetedPlanner.plan(
        command: deleteCommand("北京", .first), remainingText: "北京和北京",
        fieldText: "北京和北京", caretUTF16: 5, selectionLength: 0) else {
        check(false, "计划：第一个应成功")
        return
    }
    checkEqual(firstPlan.occurrence.index, 0, "计划：第一个 occurrence.index")
    checkEqual(firstPlan.newRemainingText, "和北京", "计划：第一个 newRemaining")
    guard case .plan(let lastPlan) = VoiceTargetedPlanner.plan(
        command: replaceCommand("北京", "上海", .last), remainingText: "北京和北京",
        fieldText: "北京和北京", caretUTF16: 5, selectionLength: 0) else {
        check(false, "计划：最后一个应成功")
        return
    }
    checkEqual(lastPlan.occurrence.index, 1, "计划：最后一个 occurrence.index")
    checkEqual(lastPlan.newRemainingText, "北京和上海", "计划：最后一个 newRemaining")

    // 无匹配 → notFound（即使字段完美）。
    expectTargetedPlanFailure(
        command: deleteCommand("上海"), remainingText: "我去北京上班",
        fieldText: "我去北京上班", caretUTF16: 6, selectionLength: 0,
        .notFound, "计划：目标不存在 → notFound")

    // 光标/选区/不可验证闸（任何 verify_mode 语义下 targeted 都强制——planner 无模式参数）。
    expectTargetedPlanFailure(
        command: deleteCommand("北京"), remainingText: "我去北京上班",
        fieldText: "我去北京上班", caretUTF16: 3, selectionLength: 0,
        .caretNotEnd, "计划：光标在中间 → caretNotEnd")
    expectTargetedPlanFailure(
        command: deleteCommand("北京"), remainingText: "我去北京上班",
        fieldText: "我去北京上班", caretUTF16: 0, selectionLength: 6,
        .selectionPresent, "计划：选区非空 → selectionPresent")
    expectTargetedPlanFailure(
        command: deleteCommand("北京"), remainingText: "我去北京上班",
        .verifyUnavailable, "计划：不可验证 → verifyUnavailable")

    // 连续两次编辑（计划级）：删「北京」→「我去上班」，再删「上班」→「我去」。
    guard case .plan(let secondPlan) = VoiceTargetedPlanner.plan(
        command: deleteCommand("上班"), remainingText: deletePlan.newRemainingText,
        fieldText: "我去上班", caretUTF16: 4, selectionLength: 0) else {
        check(false, "计划：连续第二次编辑应成功")
        return
    }
    checkEqual(secondPlan.newRemainingText, "我去", "计划：连续第二次 newRemaining")

    // emoji 目标：选区 UTF-16 长度 = 2（一次平台选区覆盖整个 grapheme）。
    guard case .plan(let emojiPlan) = VoiceTargetedPlanner.plan(
        command: deleteCommand("🙂"), remainingText: "好🙂吗",
        fieldText: "好🙂吗", caretUTF16: 4, selectionLength: 0) else {
        check(false, "计划：emoji 删除应成功")
        return
    }
    checkEqual(emojiPlan.occurrence.utf16Start, 1, "计划：emoji utf16Start")
    checkEqual(emojiPlan.occurrence.utf16Length, 2, "计划：emoji utf16Length=2")
    checkEqual(emojiPlan.newRemainingText, "好吗", "计划：emoji newRemaining")

    // remaining 自身以换行结尾（口述多行文本）：pressedEnter 优先按 remaining+"\n"
    // 判定，避免误配裸后缀（与协调器拆解同顺序，对齐 Windows）。
    guard case .plan(let newlineRemainingPlan) = VoiceTargetedPlanner.plan(
        command: deleteCommand("你好"), remainingText: "你好\n", pressedEnter: true,
        fieldText: "你好\n\n", caretUTF16: 4, selectionLength: 0) else {
        check(false, "计划：remaining 换行结尾 + auto_enter 应通过")
        return
    }
    checkEqual(newlineRemainingPlan.newRemainingText, "\n", "计划：删后 remaining 为换行")
    checkEqual(newlineRemainingPlan.occurrence.utf16Start, 0, "计划：换行结尾目标偏移")
    // 无 auto_enter 换行（字段恰为 remaining 裸后缀）同样通过。
    guard case .plan(let bareNewlinePlan) = VoiceTargetedPlanner.plan(
        command: deleteCommand("你好"), remainingText: "你好\n", pressedEnter: true,
        fieldText: "你好\n", caretUTF16: 3, selectionLength: 0) else {
        check(false, "计划：remaining 裸后缀应通过")
        return
    }
    checkEqual(bareNewlinePlan.newRemainingText, "\n", "计划：裸后缀删后 remaining")
    // pressedEnter=false 但字段多一个换行 → 换行来历不明，拒绝。
    expectTargetedPlanFailure(
        command: deleteCommand("你好"), remainingText: "你好\n", pressedEnter: false,
        fieldText: "你好\n\n", caretUTF16: 4, selectionLength: 0,
        .fieldMismatch, "计划：未记录换行（remaining 带换行场景）fieldMismatch")
}

private func expectTargetedPlanFailure(
    command: VoiceTargetedCommand, remainingText: String, pressedEnter: Bool = false,
    fieldText: String? = nil, caretUTF16: Int? = nil, selectionLength: Int? = nil,
    _ expected: VoiceTargetedFailureCode, _ name: String) {
    let result = VoiceTargetedPlanner.plan(
        command: command, remainingText: remainingText, pressedEnter: pressedEnter,
        fieldText: fieldText, caretUTF16: caretUTF16, selectionLength: selectionLength)
    guard case .failure(let code) = result else {
        check(false, "\(name)（期望拒绝 \(expected.rawValue)，实际得到计划）")
        return
    }
    checkEqual(code, expected, name)
}

// ---- 入口 ----

func runVoiceTargetedEditTests() {
    runTargetedParserPositiveTests()
    runTargetedParserNegativeTests()
    runTargetedMatcherTests()
    runTargetedPlannerTests()
}
