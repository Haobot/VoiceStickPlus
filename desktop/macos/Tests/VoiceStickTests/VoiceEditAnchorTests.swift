import Foundation
import VoiceStickCore

// 字段内定位器（语音编辑批次 1）单测——T1-T13 双端同向量表（PROMPT §7）。
// Windows tests/core_tests_voice_edit.cc 的 TestVoiceEditAnchor* 使用同一组向量
//（延续双端同向量约定）；T14/T15 为 macOS 协调器级用例（CoordinatorFsmTests），
// Windows 无 AX 侧对应（仅纯逻辑可达的 stage 日志点）。

private func expectAnchor(
    _ result: VoiceEditAnchorResult,
    kind: VoiceEditAnchor.Kind, anchorStart: Int, anchorEnd: Int, _ name: String
) {
    guard case .anchored(let anchor) = result else {
        check(false, "\(name)（期望锚定 \(kind)，实际 \(result)）")
        return
    }
    checkEqual(anchor.kind, kind, "\(name)：锚种类")
    checkEqual(anchor.anchorStart, anchorStart, "\(name)：anchorStart")
    checkEqual(anchor.anchorEnd, anchorEnd, "\(name)：anchorEnd")
}

/// locateFieldEnd（P1c 专用接口）重载：Optional 直判。
private func expectAnchorOptional(
    _ anchor: VoiceEditAnchor?,
    kind: VoiceEditAnchor.Kind, anchorStart: Int, anchorEnd: Int, _ name: String
) {
    guard let anchor else {
        check(false, "\(name)（期望锚定 \(kind)，实际 nil）")
        return
    }
    checkEqual(anchor.kind, kind, "\(name)：锚种类")
    checkEqual(anchor.anchorStart, anchorStart, "\(name)：anchorStart")
    checkEqual(anchor.anchorEnd, anchorEnd, "\(name)：anchorEnd")
}

private func expectAnchorFailure(
    _ result: VoiceEditAnchorResult, _ code: VoiceEditAnchorCode, _ name: String
) {
    guard case .failure(let actual) = result else {
        check(false, "\(name)（期望失败 \(code.rawValue)，实际 \(result)）")
        return
    }
    checkEqual(actual, code, name)
}

func runVoiceEditAnchorTests() {
    // ---- 定位器四级锚 ----

    // T1 光标紧邻段尾（P1a）：句中插入，段「北京」插在「我去」与「上班」之间，
    // 光标停在段尾 → 前缀 [0,4) 以 anchored 结尾。
    expectAnchor(
        VoiceEditAnchor.locate(
            remaining: "北京", pressedEnter: false,
            fieldText: "我去北京上班", caretUTF16: 4, selectionLength: 0),
        kind: .caretPrefix, anchorStart: 2, anchorEnd: 4,
        "T1：光标紧邻段尾 P1a")

    // T2 光标紧邻段首（P1b）：光标停在段首 → 后缀 [2,6) 以 anchored 开头。
    expectAnchor(
        VoiceEditAnchor.locate(
            remaining: "北京", pressedEnter: false,
            fieldText: "我去北京上班", caretUTF16: 2, selectionLength: 0),
        kind: .caretSuffix, anchorStart: 2, anchorEnd: 4,
        "T2：光标紧邻段首 P1b")

    // T3 字段末尾追加（P1c，回归底线）：光标在末尾 + 完整后缀。
    expectAnchor(
        VoiceEditAnchor.locate(
            remaining: "上班", pressedEnter: false,
            fieldText: "我去北京上班", caretUTF16: 6, selectionLength: 0),
        kind: .caretPrefix, anchorStart: 4, anchorEnd: 6,
        "T3：字段末尾追加（P1a 同位命中，anchorEnd=字段末尾）")
    // P1c 裸后缀：光标不在末尾（别处）也能字段末尾锚定。
    expectAnchor(
        VoiceEditAnchor.locate(
            remaining: "上班", pressedEnter: false,
            fieldText: "我去北京上班", caretUTF16: 0, selectionLength: 0),
        kind: .fieldEnd, anchorStart: 4, anchorEnd: 6,
        "T3b：光标在字段开头 → P1c 字段末尾锚")
    // P1c 已记录换行优先：field 以 remaining+\n 结尾 → 锚段含换行（len=remaining+1）。
    expectAnchorOptional(
        VoiceEditAnchor.locateFieldEnd(remaining: "上班", pressedEnter: true,
                                       fieldText: "我去北京上班\n"),
        kind: .fieldEnd, anchorStart: 4, anchorEnd: 7,
        "T3c：P1c 已记录换行优先（锚段含 \\n）")
    // P1c 裸回落：pressedEnter 但用户已手删换行 → 锚段=remaining。
    expectAnchorOptional(
        VoiceEditAnchor.locateFieldEnd(remaining: "上班", pressedEnter: true,
                                       fieldText: "我去北京上班"),
        kind: .fieldEnd, anchorStart: 4, anchorEnd: 6,
        "T3d：P1c 裸 remaining 回落（换行已手删）")
    checkNil({ () -> VoiceEditAnchor? in
        VoiceEditAnchor.locateFieldEnd(remaining: "上班", pressedEnter: true,
                                       fieldText: "我去上海\n") }(),
        "T3e：P1c 全 miss（字段无该段）→ nil")

    // T4 段唯一出现（P2）：光标在别处、段不在字段末尾且恰好出现一次。
    expectAnchor(
        VoiceEditAnchor.locate(
            remaining: "北京", pressedEnter: false,
            fieldText: "我在杭州北京出差", caretUTF16: 7, selectionLength: 0),
        kind: .uniqueOccurrence, anchorStart: 4, anchorEnd: 6,
        "T4：段唯一出现 P2（光标在别处）")

    // T5 段出现两次且 P1 全 miss → anchor_ambiguous。
    expectAnchorFailure(
        VoiceEditAnchor.locate(
            remaining: "北京", pressedEnter: false,
            fieldText: "北京很好，我去北京出差", caretUTF16: 11, selectionLength: 0),
        .anchorAmbiguous, "T5：段出现两次 → anchor_ambiguous")

    // T6 用户改过段内文本（整段精确匹配全 miss）→ field_mismatch（保留旧 toast）。
    expectAnchorFailure(
        VoiceEditAnchor.locate(
            remaining: "北京", pressedEnter: false,
            fieldText: "背景很好", caretUTF16: 4, selectionLength: 0),
        .fieldMismatch, "T6：整段精确匹配全 miss → field_mismatch")

    // T7 pressedEnter 已记录换行 + P1a：anchored 含 \n 语义正确
    //（光标在「换行之后」＝段含换行的末尾）。
    expectAnchor(
        VoiceEditAnchor.locate(
            remaining: "上班", pressedEnter: true,
            fieldText: "我去北京上班\n吗", caretUTF16: 7, selectionLength: 0),
        kind: .caretPrefix, anchorStart: 4, anchorEnd: 7,
        "T7：P1a 含已记录换行（段=上班\\n）")

    // T8 anchored 自身以 \n 结尾（口述多行）：不误并前缀——anchored 精确匹配，
    // dictated 换行属于段本身，前缀从段外起算（对齐 §17.5 返修 4）。
    expectAnchor(
        VoiceEditAnchor.locate(
            remaining: "第一行\n", pressedEnter: false,
            fieldText: "序第一行\n尾", caretUTF16: 5, selectionLength: 0),
        kind: .caretPrefix, anchorStart: 1, anchorEnd: 5,
        "T8：anchored 以口述换行结尾（P1a 精确段界）")
    // pressedEnter 且 remaining 以 \n 结尾：anchored=remaining+\n 双换行，字段只有
    // 一个换行 → P1c 裸回落按 remaining 整段（不吞口述换行）。
    expectAnchorOptional(
        VoiceEditAnchor.locateFieldEnd(remaining: "你好\n", pressedEnter: true,
                                       fieldText: "序你好\n"),
        kind: .fieldEnd, anchorStart: 1, anchorEnd: 4,
        "T8b：P1c 裸回落锚段=remaining（口述换行保留在段内）")

    // T11 替换目标跨 surrogate 对（emoji）：UTF-16 偏移正确（🙂 = 2 码元）。
    expectAnchor(
        VoiceEditAnchor.locate(
            remaining: "🙂好", pressedEnter: false,
            fieldText: "你🙂好呀", caretUTF16: 4, selectionLength: 0),
        kind: .caretPrefix, anchorStart: 1, anchorEnd: 4,
        "T11：emoji surrogate 对的 UTF-16 偏移")
    expectAnchor(
        VoiceEditAnchor.locate(
            remaining: "🇨🇳国", pressedEnter: false,
            fieldText: "我爱🇨🇳国", caretUTF16: 7, selectionLength: 0),
        kind: .caretPrefix, anchorStart: 2, anchorEnd: 7,
        "T11b：国旗 ZWJ 序列（🇨🇳=4 码元）偏移")

    // 补充：选区非空 → P1a/P1b 跳过，仍可 P1c/P2。
    expectAnchor(
        VoiceEditAnchor.locate(
            remaining: "上班", pressedEnter: false,
            fieldText: "我去北京上班", caretUTF16: 2, selectionLength: 2),
        kind: .fieldEnd, anchorStart: 4, anchorEnd: 6,
        "补充：选区非空跳过 P1a/P1b → P1c")
    // 补充：空 remaining（防御）→ field_mismatch。
    expectAnchorFailure(
        VoiceEditAnchor.locate(
            remaining: "", pressedEnter: false,
            fieldText: "任意", caretUTF16: 1, selectionLength: 0),
        .fieldMismatch, "补充：空 remaining → field_mismatch")
    // 补充：P1a 优先于 P1b/P1c（同一 anchored 多级可命中时取最先）。
    expectAnchor(
        VoiceEditAnchor.locate(
            remaining: "你好", pressedEnter: false,
            fieldText: "你好你好", caretUTF16: 2, selectionLength: 0),
        kind: .caretPrefix, anchorStart: 0, anchorEnd: 2,
        "补充：P1a 固定最优先")

    // ---- T13 句中插入去句末句号（stripMidlinePeriod/isMidlineInsertion）----

    checkEqual(VoiceEditAnchor.stripMidlinePeriod(text: "今天天气不错。", isMidline: true),
               "今天天气不错", "T13：句中标准剥离（。）")
    checkEqual(VoiceEditAnchor.stripMidlinePeriod(text: "今天天气不错．", isMidline: true),
               "今天天气不错", "T13：全角句点（．）剥离")
    checkEqual(VoiceEditAnchor.stripMidlinePeriod(text: "收到.", isMidline: true),
               "收到", "T13：半角句点（前字符非 ASCII 字母数字）剥离")
    checkEqual(VoiceEditAnchor.stripMidlinePeriod(text: "真的吗！", isMidline: true),
               "真的吗！", "T13：！保留")
    checkEqual(VoiceEditAnchor.stripMidlinePeriod(text: "是吗？", isMidline: true),
               "是吗？", "T13：？保留")
    checkEqual(VoiceEditAnchor.stripMidlinePeriod(text: "一共3.", isMidline: true),
               "一共3.", "T13：3. 的句点保留（数字）")
    checkEqual(VoiceEditAnchor.stripMidlinePeriod(text: "升级到v1.", isMidline: true),
               "升级到v1.", "T13：v1. 的句点保留（字母）")
    checkEqual(VoiceEditAnchor.stripMidlinePeriod(text: "句话.", isMidline: true),
               "句话", "T13：CJK 后半角句点剥离")
    checkEqual(VoiceEditAnchor.stripMidlinePeriod(text: "今天天气不错。", isMidline: false),
               "今天天气不错。", "T13：非句中不剥")
    checkEqual(VoiceEditAnchor.stripMidlinePeriod(text: "", isMidline: true),
               "", "T13：空文本原样")
    checkEqual(VoiceEditAnchor.midlinePeriodStrippable("不错。"), true,
               "T13 预检：句号结尾可剥候选")
    checkEqual(VoiceEditAnchor.midlinePeriodStrippable("3."), false,
               "T13 预检：数字句点非候选")
    checkEqual(VoiceEditAnchor.midlinePeriodStrippable("不错！"), false,
               "T13 预检：！非候选")

    // 裸句号守卫（与标点独词输入的交叉修复：单独说「句号」→ 注入文本恰为「。」）：
    // 句号之前去掉首尾空白后为空 → 必然是刻意注入的标点，句中也永不剥（否则剥成
    // 空串、什么都没插进去）；句号前有实际内容照剥。预检同口径（裸句号不读字段）。
    checkEqual(VoiceEditAnchor.stripMidlinePeriod(text: "。", isMidline: true),
               "。", "T13 裸句号：「。」句中原样保留")
    checkEqual(VoiceEditAnchor.stripMidlinePeriod(text: "．", isMidline: true),
               "．", "T13 裸句号：「．」句中原样保留")
    checkEqual(VoiceEditAnchor.stripMidlinePeriod(text: ".", isMidline: true),
               ".", "T13 裸句号：「.」句中原样保留")
    checkEqual(VoiceEditAnchor.stripMidlinePeriod(text: " 。", isMidline: true),
               " 。", "T13 裸句号：仅空白+「。」原样保留")
    checkEqual(VoiceEditAnchor.stripMidlinePeriod(text: "\n.", isMidline: true),
               "\n.", "T13 裸句号：仅换行+「.」原样保留")
    checkEqual(VoiceEditAnchor.stripMidlinePeriod(text: "今天很好。", isMidline: true),
               "今天很好", "T13 裸句号守卫：句号前有内容照剥")
    checkEqual(VoiceEditAnchor.midlinePeriodStrippable("。"), false,
               "T13 预检：裸「。」非候选")
    checkEqual(VoiceEditAnchor.midlinePeriodStrippable("．"), false,
               "T13 预检：裸「．」非候选")
    checkEqual(VoiceEditAnchor.midlinePeriodStrippable("."), false,
               "T13 预检：裸「.」非候选")
    checkEqual(VoiceEditAnchor.midlinePeriodStrippable(" 。"), false,
               "T13 预检：仅空白+「。」非候选")
    checkEqual(VoiceEditAnchor.midlinePeriodStrippable("\n."), false,
               "T13 预检：仅换行+「.」非候选")
    checkEqual(VoiceEditAnchor.midlinePeriodStrippable("今天很好。"), true,
               "T13 预检：句号前有内容仍是候选")

    // isMidlineInsertion：光标后同一逻辑行还有非空白字符。
    check(VoiceEditAnchor.isMidlineInsertion(fieldText: "你好|世界", caretUTF16: 2),
          "T13 句中判定：光标后有非空白字符")
    check(!VoiceEditAnchor.isMidlineInsertion(fieldText: "你好世界", caretUTF16: 4),
          "T13 句中判定：光标在末尾非句中")
    check(!VoiceEditAnchor.isMidlineInsertion(fieldText: "你好\n世界", caretUTF16: 2),
          "T13 句中判定：光标后是换行非句中")
    check(!VoiceEditAnchor.isMidlineInsertion(fieldText: "你好  世界", caretUTF16: 2),
          "T13 句中判定：光标后是空格非句中")
    check(!VoiceEditAnchor.isMidlineInsertion(fieldText: "你好世界", caretUTF16: -1),
          "T13 句中判定：非法 caret 非句中")
    // 翻译模式样本（判定只看插入位置，不看模式；zh 目标译文以 。 结尾）。
    // 注：英文 "today." 的句点因前字符是 ASCII 字母被保留（数字/版本号保护的
    // 既定边界，双端一致）；中文译文句号正常剥离。
    checkEqual(VoiceEditAnchor.stripMidlinePeriod(text: "The weather is nice today.", isMidline: true),
               "The weather is nice today.",
               "T13：翻译模式英文样本（ASCII 字母后句点保留）")
    checkEqual(VoiceEditAnchor.stripMidlinePeriod(text: "今天天气很好。", isMidline: true),
               "今天天气很好",
               "T13：翻译模式中文样本（译文句号剥离）")
}
