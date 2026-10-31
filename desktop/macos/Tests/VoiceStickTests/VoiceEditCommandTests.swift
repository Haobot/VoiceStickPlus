import Foundation
import VoiceStickCore

// 语音编辑指令解析器 + 删除计划单测（Doc/Plan/voice-edit-commands.md §11.1）。
// 覆盖：正例 >= 30 / 负例 >= 20 / 删除计划向量 >= 15；与 Windows
// tests/core_tests_voice_edit.cc 使用同一张语法表与同一份向量（双端同向量约定）。
// 运行：cd desktop/macos && swift run VoiceStickTests。

// ---- 断言辅助 ----

private func expectEditCommand(_ text: String, _ expected: VoiceEditCommand,
                               defaultChars: Int = 1, maxChars: Int = 20,
                               _ name: String) {
    checkEqual(VoiceEditParser.parse(text, defaultChars: defaultChars, maxChars: maxChars),
               expected, name)
}

private func expectNoEditCommand(_ text: String, defaultChars: Int = 1, maxChars: Int = 20,
                                 _ name: String) {
    checkNil(VoiceEditParser.parse(text, defaultChars: defaultChars, maxChars: maxChars), name)
}

private func expectDeletionPlan(_ result: VoiceEditPlanResult, count: Int, charCount: Int,
                                newlineCount: Int, remaining: String,
                                verification: VoiceEditVerification, _ name: String) {
    guard case .plan(let plan) = result else {
        check(false, "\(name)（期望删除计划，实际 \(result)）")
        return
    }
    checkEqual(plan.count, count, "\(name)：退格次数")
    checkEqual(plan.charCount, charCount, "\(name)：删除字数")
    checkEqual(plan.newlineCount, newlineCount, "\(name)：换行删除数")
    checkEqual(plan.newRemainingText, remaining, "\(name)：新 remainingText")
    checkEqual(plan.verification, verification, "\(name)：verified/unverified")
    checkEqual(plan.newPressedEnter, false, "\(name)：新 pressedEnter")
}

private func expectDeletionFailure(_ result: VoiceEditPlanResult,
                                   _ expected: VoiceEditFailureCode, _ name: String) {
    guard case .failure(let code) = result else {
        check(false, "\(name)（期望拒绝 \(expected.rawValue)，实际 \(result)）")
        return
    }
    checkEqual(code, expected, name)
}

private func charsCommand(_ count: Int, _ source: VoiceEditCommandSource) -> VoiceEditCommand {
    VoiceEditCommand(action: .deleteLastChars(count), source: source)
}

private func sentencesCommand(_ count: Int, _ source: VoiceEditCommandSource) -> VoiceEditCommand {
    VoiceEditCommand(action: .deleteLastSentences(count), source: source)
}

private let kAllCommand = VoiceEditCommand(action: .deleteAll, source: .all)

// ---- 解析器正例（>= 30）----

private func runVoiceEditParserPositiveTests() {
    // 单独「删除/删掉/去掉」→ 默认 1 字（决策点 D1）。
    expectEditCommand("删除", charsCommand(1, .defaultCount), "正例：单独删除")
    expectEditCommand("删掉", charsCommand(1, .defaultCount), "正例：删掉")
    expectEditCommand("去掉", charsCommand(1, .defaultCount), "正例：去掉")
    expectEditCommand("删掉一下", charsCommand(1, .defaultCount), "正例：删掉一下")
    expectEditCommand("删除一下", charsCommand(1, .defaultCount), "正例：删除一下")
    expectEditCommand("请删除", charsCommand(1, .defaultCount), "正例：礼貌前缀+单独删除")

    // 删最后 N 个字（中文数字/阿拉伯数字/两/俩、字/字符/汉字/文字）。
    expectEditCommand("删掉最后两个字", charsCommand(2, .explicitCount), "正例：删掉最后两个字")
    expectEditCommand("删除两个字", charsCommand(2, .explicitCount), "正例：删除两个字")
    expectEditCommand("删掉最后 2 个字", charsCommand(2, .explicitCount), "正例：阿拉伯数字+空白")
    expectEditCommand("去掉最后两个字符", charsCommand(2, .explicitCount), "正例：字符单位")
    expectEditCommand("删除最后两个汉字", charsCommand(2, .explicitCount), "正例：汉字单位")
    expectEditCommand("删除最后两个文字", charsCommand(2, .explicitCount), "正例：文字单位")
    expectEditCommand("删除十二个字", charsCommand(12, .explicitCount), "正例：十二")
    expectEditCommand("删除二十个字", charsCommand(20, .explicitCount), "正例：二十（上限）")
    expectEditCommand("删除最后俩字", charsCommand(2, .explicitCount), "正例：俩=2")

    // 把字句 / 后置动词 / 常见句式（D6）。
    expectEditCommand("最后两个字删掉", charsCommand(2, .explicitCount), "正例：后置动词")
    expectEditCommand("把最后两个字删掉", charsCommand(2, .explicitCount), "正例：把字句")
    expectEditCommand("把最后两个字删除", charsCommand(2, .explicitCount), "正例：把字句+删除")

    // 删最后 N 句（含「这句话」「一句话」等变体）。
    expectEditCommand("删掉最后一句", sentencesCommand(1, .explicitCount), "正例：删最后一句（「一」显式）")
    expectEditCommand("删除最后这句话", sentencesCommand(1, .defaultCount), "正例：最后这句话")
    expectEditCommand("删掉最后两句话", sentencesCommand(2, .explicitCount), "正例：最后两句话")
    expectEditCommand("删除两句话", sentencesCommand(2, .explicitCount), "正例：两句话")
    expectEditCommand("把最后一句删掉", sentencesCommand(1, .explicitCount), "正例：把最后一句删掉")
    expectEditCommand("把最后两句话删掉", sentencesCommand(2, .explicitCount), "正例：把最后两句话删掉")

    // 整段删除（撤销/撤回 = 本次输入整段，决策点 D1）。
    expectEditCommand("删除刚才的", kAllCommand, "正例：删除刚才的")
    expectEditCommand("删掉全部", kAllCommand, "正例：删掉全部")
    expectEditCommand("全部删掉", kAllCommand, "正例：全部删掉")
    expectEditCommand("删除本次输入", kAllCommand, "正例：删除本次输入")
    expectEditCommand("清空", kAllCommand, "正例：清空")
    expectEditCommand("撤销", kAllCommand, "正例：撤销=整段")
    expectEditCommand("撤回", kAllCommand, "正例：撤回=整段")
    expectEditCommand("都删掉", kAllCommand, "正例：都删掉")
    expectEditCommand("删除刚才说的", kAllCommand, "正例：删除刚才说的")
    expectEditCommand("把刚才的删掉", kAllCommand, "正例：把刚才的删掉")

    // 归一化：首尾空白/尾部标点/全角数字/全角空格。
    expectEditCommand("删除最后两个字。", charsCommand(2, .explicitCount), "正例：尾部句号")
    expectEditCommand("删除最后两个字。！？", charsCommand(2, .explicitCount), "正例：叠加尾部标点")
    expectEditCommand("\u{3000}删除最后两个字\u{3000}", charsCommand(2, .explicitCount), "正例：全角空格包围")
    expectEditCommand("  删除最后两个字。 ", charsCommand(2, .explicitCount), "正例：半角空白+句号")
    expectEditCommand("删除最后\u{FF12}个字", charsCommand(2, .explicitCount), "正例：全角数字")

    // 位置词变体。
    expectEditCommand("删掉末尾两个字", charsCommand(2, .explicitCount), "正例：末尾")
    expectEditCommand("删除结尾2个字", charsCommand(2, .explicitCount), "正例：结尾")
    expectEditCommand("去掉后面两个字符", charsCommand(2, .explicitCount), "正例：后面")

    // 礼貌前缀 / 语气尾。
    expectEditCommand("请删除最后两个字", charsCommand(2, .explicitCount), "正例：请前缀")
    expectEditCommand("帮我删掉最后一句", sentencesCommand(1, .explicitCount), "正例：帮我前缀")
    expectEditCommand("麻烦删除", charsCommand(1, .defaultCount), "正例：麻烦前缀")
    expectEditCommand("请帮我删除最后两个字", charsCommand(2, .explicitCount), "正例：请帮我前缀")
    expectEditCommand("删除最后两个字吧", charsCommand(2, .explicitCount), "正例：吧语气尾")
    expectEditCommand("删掉最后两个字啊", charsCommand(2, .explicitCount), "正例：啊语气尾")
    expectEditCommand("删除最后一句呀", sentencesCommand(1, .explicitCount), "正例：呀语气尾")

    // 其它白名单动词（带数量）。
    expectEditCommand("移除最后两个字符", charsCommand(2, .explicitCount), "正例：移除")
    expectEditCommand("清除最后两个字", charsCommand(2, .explicitCount), "正例：清除")
    expectEditCommand("删去最后两个字", charsCommand(2, .explicitCount), "正例：删去")
    expectEditCommand("取消最后两个字", charsCommand(2, .explicitCount), "正例：取消")
    expectEditCommand("删两个字", charsCommand(2, .explicitCount), "正例：单独删+数量")

    // 配置参数：default_chars / max_chars。
    expectEditCommand("删除", charsCommand(3, .defaultCount), defaultChars: 3,
                      "正例：default_chars=3")
    expectEditCommand("删除", charsCommand(1, .defaultCount), defaultChars: 0,
                      "正例：default_chars 越界回落 1")
    expectEditCommand("删除", charsCommand(20, .defaultCount), defaultChars: 50, maxChars: 20,
                      "正例：default_chars 超上限 clamp")
    expectEditCommand("删除最后两个字", charsCommand(2, .explicitCount), defaultChars: 5,
                      "正例：显式数量不受 default_chars 影响")
    expectEditCommand("删除三个字", charsCommand(3, .explicitCount), maxChars: 3,
                      "正例：max_chars=3 边界")
}

// ---- 解析器负例（>= 20）----

private func runVoiceEditParserNegativeTests() {
    // 混合正文 / 包含式 / 否定 / 问句：一律按普通文本上屏。
    expectNoEditCommand("请把删除按钮改成红色", "负例：混合正文")
    expectNoEditCommand("你好世界删除最后两个字", "负例：正文+指令混说")
    expectNoEditCommand("把文件删除", "负例：包含式把字句")
    expectNoEditCommand("把所有内容删除", "负例：包含式所有内容")
    expectNoEditCommand("别删除", "负例：否定别删除")
    expectNoEditCommand("不要删除最后两个字", "负例：否定不要删除")
    expectNoEditCommand("要不要删除", "负例：疑问句")
    expectNoEditCommand("删不删", "负例：删不删")
    expectNoEditCommand("删除功能", "负例：删除功能（无单位）")
    expectNoEditCommand("清空输入框", "负例：清空输入框")
    expectNoEditCommand("撤销删除", "负例：撤销删除")
    expectNoEditCommand("删掉这个文件", "负例：删掉这个文件")
    expectNoEditCommand("删除最后两个字然后粘贴", "负例：指令后夹带正文")
    expectNoEditCommand("最后两个字删掉了吗", "负例：疑问语气尾")
    expectNoEditCommand("删掉最后两个字符呢", "负例：呢不是识别语气尾")

    // 不支持的位置词/单位/动作。
    expectNoEditCommand("删除前面两个字", "负例：前面（一期不支持）")
    expectNoEditCommand("删除开头两个字", "负例：开头（一期不支持）")
    expectNoEditCommand("删除第一个字", "负例：第一（一期不支持）")
    expectNoEditCommand("删除最后一段", "负例：段单位（一期不支持）")
    expectNoEditCommand("删除最后一行", "负例：行单位（一期不支持）")
    expectNoEditCommand("删除最后一个词", "负例：词单位（一期不支持）")
    expectNoEditCommand("删除最后两个", "负例：缺单位")

    // 数量越界/非法。
    expectNoEditCommand("删除 100 个字", "负例：超 max_chars")
    expectNoEditCommand("删除二十一个字", "负例：21 超默认上限 20")
    expectNoEditCommand("删除零个字", "负例：0 无效")
    expectNoEditCommand("删除五个字", maxChars: 3, "负例：5 超 max_chars=3")
    expectNoEditCommand("删除 2 个文件", "负例：文件不是字单位")

    // 空/纯标点/单独「删」/单独「移除」。
    expectNoEditCommand("", "负例：空串")
    expectNoEditCommand("   ", "负例：纯空白")
    expectNoEditCommand("。。。", "负例：纯标点")
    expectNoEditCommand("删", "负例：单独删（只在带数量时接受）")
    expectNoEditCommand("移除", "负例：单独移除不识别")

    // 长度上限（kVoiceEditMaxCommandChars=24）：剥掉语气尾后本可命中，超长即不识别。
    let longTail = "删除最后两个字" + String(repeating: "吧", count: 20)
    expectNoEditCommand(longTail, "负例：超 24 字符命令文本")
}

// ---- 删除计划向量（>= 15，方案 §6.3）----

private func runVoiceEditPlannerTests() {
    // V1：U1 基准——字段完整 + auto_enter 换行 + 光标在末尾 → 3 次退格。
    let u1Field = "你好世界\n"
    expectDeletionPlan(
        VoiceEditPlanner.plan(
            remainingText: "你好世界", pressedEnter: true, action: .deleteLastChars(2),
            fieldText: u1Field, caretUTF16: u1Field.utf16.count, selectionLength: 0),
        count: 3, charCount: 2, newlineCount: 1, remaining: "你好", verification: .verified,
        "计划：U1 删最后两个字含换行")

    // V2：有换行但只删 1 字。
    expectDeletionPlan(
        VoiceEditPlanner.plan(
            remainingText: "你好世界", pressedEnter: true, action: .deleteLastChars(1),
            fieldText: u1Field, caretUTF16: u1Field.utf16.count, selectionLength: 0),
        count: 2, charCount: 1, newlineCount: 1, remaining: "你好世", verification: .verified,
        "计划：删 1 字含换行")

    // V3：pressedEnter=true 但字段没有换行（单行输入框吞掉 Return）→ 不多发退格。
    expectDeletionPlan(
        VoiceEditPlanner.plan(
            remainingText: "你好世界", pressedEnter: true, action: .deleteLastChars(2),
            fieldText: "你好世界", caretUTF16: 4, selectionLength: 0),
        count: 2, charCount: 2, newlineCount: 0, remaining: "你好", verification: .verified,
        "计划：字段无换行则不删换行")

    // V4：用户手动删过尾部——eff 截断为字段实际内容。
    expectDeletionPlan(
        VoiceEditPlanner.plan(
            remainingText: "你好世界", pressedEnter: true, action: .deleteLastChars(2),
            fieldText: "你好世\n", caretUTF16: 4, selectionLength: 0),
        count: 3, charCount: 2, newlineCount: 1, remaining: "你", verification: .verified,
        "计划：用户已手动删 1 字 → eff 截断")

    // V5：用户继续手打内容 → 字段尾部不匹配，拒绝。
    expectDeletionFailure(
        VoiceEditPlanner.plan(
            remainingText: "你好世界", pressedEnter: true, action: .deleteLastChars(2),
            fieldText: "你好世界\n吗", caretUTF16: 6, selectionLength: 0),
        .fieldMismatch, "计划：用户追加手打内容 → field_mismatch")

    // V6：光标移到中间 → 拒绝（即使字段文本完整）。
    expectDeletionFailure(
        VoiceEditPlanner.plan(
            remainingText: "你好世界", pressedEnter: false, action: .deleteLastChars(1),
            fieldText: "你好世界", caretUTF16: 2, selectionLength: 0),
        .caretNotEnd, "计划：光标在中间 → caret_not_end")

    // V7：存在选区 → 拒绝（提示先取消选中）。
    expectDeletionFailure(
        VoiceEditPlanner.plan(
            remainingText: "你好世界", pressedEnter: false, action: .deleteLastChars(1),
            fieldText: "你好世界", caretUTF16: 0, selectionLength: 4),
        .selectionPresent, "计划：存在选区 → selection_present")

    // V8：strict 模式读不到字段 → 拒绝。
    expectDeletionFailure(
        VoiceEditPlanner.plan(
            remainingText: "你好世界", pressedEnter: true, action: .deleteLastChars(1),
            verifyMode: .strict),
        .verifyUnavailable, "计划：strict 不可验证 → verify_unavailable")

    // V9：auto 模式不可验证且不在 unverified 窗口 → 拒绝。
    expectDeletionFailure(
        VoiceEditPlanner.plan(
            remainingText: "你好世界", pressedEnter: true, action: .deleteLastChars(1),
            verifyMode: .auto, unverifiedWindowOpen: false),
        .verifyUnavailable, "计划：auto 超 unverified 窗口 → verify_unavailable")

    // V10：auto 模式不可验证但在 8s 窗口内 → 盲删（unverified，含换行）。
    expectDeletionPlan(
        VoiceEditPlanner.plan(
            remainingText: "你好世界", pressedEnter: true, action: .deleteLastChars(2),
            verifyMode: .auto, unverifiedWindowOpen: true),
        count: 3, charCount: 2, newlineCount: 1, remaining: "你好", verification: .unverified,
        "计划：auto 窗口内不可验证 → unverified 注入")

    // V11：off 模式不校验，直接按 remainingText 估算。
    expectDeletionPlan(
        VoiceEditPlanner.plan(
            remainingText: "你好世界", pressedEnter: false, action: .deleteLastChars(2),
            verifyMode: .off),
        count: 2, charCount: 2, newlineCount: 0, remaining: "你好", verification: .unverified,
        "计划：verify_mode=off 不校验")

    // V12：整段删除 + 换行（撤销/撤回语义）。
    expectDeletionPlan(
        VoiceEditPlanner.plan(
            remainingText: "你好世界", pressedEnter: true, action: .deleteAll,
            fieldText: u1Field, caretUTF16: u1Field.utf16.count, selectionLength: 0),
        count: 5, charCount: 4, newlineCount: 1, remaining: "", verification: .verified,
        "计划：整段删除含换行")

    // V13：整段删除不可验证（off）。
    expectDeletionPlan(
        VoiceEditPlanner.plan(
            remainingText: "你好世界", pressedEnter: false, action: .deleteAll,
            verifyMode: .off),
        count: 4, charCount: 4, newlineCount: 0, remaining: "", verification: .unverified,
        "计划：整段删除不可验证")

    // V14：N 超过剩余 → clamp（不越过注入边界）。
    expectDeletionPlan(
        VoiceEditPlanner.plan(
            remainingText: "好", pressedEnter: false, action: .deleteLastChars(5),
            fieldText: "好", caretUTF16: 1, selectionLength: 0),
        count: 1, charCount: 1, newlineCount: 0, remaining: "", verification: .verified,
        "计划：N 超过剩余 → clamp")

    // V15：用户已删到只剩 1 字，仍按可见最后 N 字 clamp。
    expectDeletionPlan(
        VoiceEditPlanner.plan(
            remainingText: "你好世界", pressedEnter: true, action: .deleteLastChars(2),
            fieldText: "你\n", caretUTF16: 2, selectionLength: 0),
        count: 2, charCount: 1, newlineCount: 1, remaining: "", verification: .verified,
        "计划：仅剩 1 字时 clamp")

    // V16：句子切分——最后 1 句（含句末标点）。
    let sentencesText = "第一句。第二句！第三句？"
    expectDeletionPlan(
        VoiceEditPlanner.plan(
            remainingText: sentencesText, pressedEnter: false,
            action: .deleteLastSentences(1),
            fieldText: sentencesText, caretUTF16: sentencesText.utf16.count,
            selectionLength: 0),
        count: 4, charCount: 4, newlineCount: 0, remaining: "第一句。第二句！",
        verification: .verified, "计划：句子切分最后 1 句")

    // V17：句子切分——最后 2 句。
    expectDeletionPlan(
        VoiceEditPlanner.plan(
            remainingText: sentencesText, pressedEnter: false,
            action: .deleteLastSentences(2),
            fieldText: sentencesText, caretUTF16: sentencesText.utf16.count,
            selectionLength: 0),
        count: 8, charCount: 8, newlineCount: 0, remaining: "第一句。",
        verification: .verified, "计划：句子切分最后 2 句")

    // V18：无标点 → 整段算一句。
    expectDeletionPlan(
        VoiceEditPlanner.plan(
            remainingText: "你好世界", pressedEnter: false, action: .deleteLastSentences(1),
            fieldText: "你好世界", caretUTF16: 4, selectionLength: 0),
        count: 4, charCount: 4, newlineCount: 0, remaining: "", verification: .verified,
        "计划：无标点整段算一句")

    // V19：换行也是句子边界。
    let newlineSentences = "第一行\n第二行"
    expectDeletionPlan(
        VoiceEditPlanner.plan(
            remainingText: newlineSentences, pressedEnter: false,
            action: .deleteLastSentences(1),
            fieldText: newlineSentences, caretUTF16: newlineSentences.utf16.count,
            selectionLength: 0),
        count: 3, charCount: 3, newlineCount: 0, remaining: "第一行\n",
        verification: .verified, "计划：换行作为句子边界")

    // V20：句子数超过实际句数 → clamp 到整段。
    expectDeletionPlan(
        VoiceEditPlanner.plan(
            remainingText: "你好世界", pressedEnter: false, action: .deleteLastSentences(2),
            fieldText: "你好世界", caretUTF16: 4, selectionLength: 0),
        count: 4, charCount: 4, newlineCount: 0, remaining: "", verification: .verified,
        "计划：句子数超过实际句数 → clamp")

    // V21：emoji 按 grapheme 计（一个 emoji 算 1 个字）。
    let emojiText = "你好🙂🙂"
    expectDeletionPlan(
        VoiceEditPlanner.plan(
            remainingText: emojiText, pressedEnter: false, action: .deleteLastChars(2),
            fieldText: emojiText, caretUTF16: emojiText.utf16.count, selectionLength: 0),
        count: 2, charCount: 2, newlineCount: 0, remaining: "你好", verification: .verified,
        "计划：emoji 按 grapheme 计数")

    // V22：组合字符按 grapheme 计（e + 组合重音算 1 个字）。
    let combiningText = "你好e\u{301}e\u{301}"
    expectDeletionPlan(
        VoiceEditPlanner.plan(
            remainingText: combiningText, pressedEnter: false, action: .deleteLastChars(2),
            fieldText: combiningText, caretUTF16: combiningText.utf16.count,
            selectionLength: 0),
        count: 2, charCount: 2, newlineCount: 0, remaining: "你好", verification: .verified,
        "计划：组合字符按 grapheme 计数")

    // V23：remaining 为空 → no_injection（无目标）。
    expectDeletionFailure(
        VoiceEditPlanner.plan(remainingText: "", pressedEnter: true, action: .deleteAll),
        .noInjection, "计划：remaining 为空 → no_injection")

    // V24：off 模式 remaining 为空同样 no_injection。
    expectDeletionFailure(
        VoiceEditPlanner.plan(remainingText: "", pressedEnter: false,
                              action: .deleteLastChars(1), verifyMode: .off),
        .noInjection, "计划：off 且 remaining 为空 → no_injection")

    // V25：字段为空（读得到但无文本）→ field_mismatch。
    expectDeletionFailure(
        VoiceEditPlanner.plan(
            remainingText: "你好", pressedEnter: false, action: .deleteLastChars(1),
            fieldText: "", caretUTF16: 0, selectionLength: 0),
        .fieldMismatch, "计划：字段为空 → field_mismatch")

    // V26：超大字段（> 2M UTF-16）按不可验证处理：auto 无窗口拒绝。
    let hugeField = String(repeating: "a", count: kVoiceEditMaxVerifiableFieldUTF16 + 1)
    expectDeletionFailure(
        VoiceEditPlanner.plan(
            remainingText: "你好", pressedEnter: false, action: .deleteLastChars(1),
            fieldText: hugeField, caretUTF16: hugeField.utf16.count, selectionLength: 0,
            verifyMode: .auto, unverifiedWindowOpen: false),
        .verifyUnavailable, "计划：超大字段按不可验证处理")

    // V27：超大字段 + auto 窗口内 → unverified 盲删。
    expectDeletionPlan(
        VoiceEditPlanner.plan(
            remainingText: "你好", pressedEnter: false, action: .deleteLastChars(1),
            fieldText: hugeField, caretUTF16: hugeField.utf16.count, selectionLength: 0,
            verifyMode: .auto, unverifiedWindowOpen: true),
        count: 1, charCount: 1, newlineCount: 0, remaining: "你", verification: .unverified,
        "计划：超大字段窗口内 → unverified")

    // V28：不可验证的句子删除（按 remainingText 切分）。
    expectDeletionPlan(
        VoiceEditPlanner.plan(
            remainingText: "第一句。第二句。", pressedEnter: false,
            action: .deleteLastSentences(1), verifyMode: .off),
        count: 4, charCount: 4, newlineCount: 0, remaining: "第一句。",
        verification: .unverified, "计划：不可验证句子删除")

    // V29：整段删除但用户已手动删掉一部分（eff 截断后整段清空实际剩余）。
    expectDeletionPlan(
        VoiceEditPlanner.plan(
            remainingText: "你好世界", pressedEnter: true, action: .deleteAll,
            fieldText: "你好\n", caretUTF16: 3, selectionLength: 0),
        count: 3, charCount: 2, newlineCount: 1, remaining: "", verification: .verified,
        "计划：整段删除按 eff 截断")

    // V30：字段尾部恰为 remaining 前缀、但光标不在 UTF-16 末尾 → caret_not_end。
    expectDeletionFailure(
        VoiceEditPlanner.plan(
            remainingText: "你好", pressedEnter: false, action: .deleteLastChars(1),
            fieldText: "你好", caretUTF16: 3, selectionLength: 0),
        .caretNotEnd, "计划：caretUTF16 非法 → caret_not_end")

    // V31：planner 的 max_chars 纵深防御——字动作请求量被上限收紧（解析器侧本应
    // 先拒绝越界指令，planner 再兜一层，保证任何调用方单条字指令不超配置上限）。
    expectDeletionPlan(
        VoiceEditPlanner.plan(
            remainingText: "你好世界", pressedEnter: false, action: .deleteLastChars(5),
            fieldText: "你好世界", caretUTF16: 4, selectionLength: 0, maxChars: 3),
        count: 3, charCount: 3, newlineCount: 0, remaining: "你", verification: .verified,
        "计划：字动作受 max_chars 上限收紧")

    // V32：句动作不受字数上限约束（单位是句，长句一次删完仍符合语义）。
    let longSentence = "第一句。第二句非常非常长的一段话。"
    expectDeletionPlan(
        VoiceEditPlanner.plan(
            remainingText: longSentence, pressedEnter: false,
            action: .deleteLastSentences(1),
            fieldText: longSentence, caretUTF16: longSentence.utf16.count,
            selectionLength: 0, maxChars: 3),
        count: 13, charCount: 13, newlineCount: 0, remaining: "第一句。",
        verification: .verified, "计划：句动作不受 max_chars 限制")

    // 解析 → 计划联调（U1 端到端纯逻辑）。
    let parsed = VoiceEditParser.parse("删掉最后两个字。")
    if let parsed, case .plan(let plan) = VoiceEditPlanner.plan(
        remainingText: "你好世界", pressedEnter: true, action: parsed.action,
        fieldText: u1Field, caretUTF16: u1Field.utf16.count, selectionLength: 0) {
        checkEqual(plan.count, 3, "联调：U1 解析+计划 → 3 次退格")
    } else {
        check(false, "联调：U1 解析+计划应成功")
    }
}

// ---- 入口 ----

func runVoiceEditCommandTests() {
    runVoiceEditParserPositiveTests()
    runVoiceEditParserNegativeTests()
    runVoiceEditPlannerTests()
}