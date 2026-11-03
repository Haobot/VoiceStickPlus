import Foundation
import VoiceStickCore

// 语音编辑批次 2：搜索域解析（VoiceTargetedDomain）+ 光标恢复平移
//（VoiceTargetedCursorMath）单测。与 Windows tests/core_tests_voice_edit.cc 的
// batch2 段使用同一张向量表（双端同向量约定）。
// 位置限定扩展（第N个/倒数第N个）的解析/计划向量见 VoiceTargetedEditTests；
// 序数应答解析见本文件 runVoiceEditOrdinalReplyTests（交付 B）。

private func recentDomain(
    _ remaining: String, anchorStart: Int
) -> VoiceTargetedRecentDomain {
    VoiceTargetedRecentDomain(
        remainingText: remaining,
        anchor: VoiceEditAnchor(
            kind: .fieldEnd, anchorStart: anchorStart,
            anchorEnd: anchorStart + remaining.utf16.count))
}

private func plainCommand(
    _ target: String, _ position: VoiceTargetedPosition = .unspecified,
    hasExplicitScope: Bool = false
) -> VoiceTargetedCommand {
    VoiceTargetedCommand(
        action: .delete, target: target, position: position,
        hasExplicitScope: hasExplicitScope)
}

// ---- 域解析全规则表（§2.1）----

private func runDomainRuleTests() {
    let field = "手打开头北京和北京以及手打北京结尾"
    // 字段匹配：北京出现 3 处（grapheme 偏移 4、7、14 → UTF-16 同值）。

    // Tier 1 命中：无作用域无位置、recent 可用且 remaining 内恰 1 处 → lastInjection。
    let recent = recentDomain("今天去了北京一趟", anchorStart: 6)
    guard case .execute(let tier1Domain, let tier1Selected) = VoiceTargetedDomain.resolve(
        command: plainCommand("北京"), recent: recent, fieldText: field) else {
        check(false, "domain: Tier 1 命中应 execute")
        return
    }
    checkEqual(tier1Domain, .lastInjection, "domain: Tier 1 域=lastInjection")
    checkEqual(tier1Selected.utf16Start, 4, "domain: Tier 1 域内偏移（remaining 内）")

    // Tier 1 不唯一（remaining 内 2 处）→ 整框；整框 3 处 → 追问（wholeField）。
    let recentDouble = recentDomain("北京和北京", anchorStart: 4)
    guard case .clarify(let count, let clarifyDomain) = VoiceTargetedDomain.resolve(
        command: plainCommand("北京"), recent: recentDouble, fieldText: field) else {
        check(false, "domain: Tier 1 不唯一应转整框追问")
        return
    }
    checkEqual(count, 3, "domain: 整框追问计数=3")
    checkEqual(clarifyDomain, .wholeField, "domain: 追问域=wholeField")

    // Tier 1 remaining 内 0 处但整框存在 → 整框（唯一处执行）。
    let recentMiss = recentDomain("今天去了上海一趟", anchorStart: 6)
    let fieldUnique = "开头有北京结尾"
    guard case .execute(let wfDomain, let wfSelected) = VoiceTargetedDomain.resolve(
        command: plainCommand("北京"), recent: recentMiss, fieldText: fieldUnique) else {
        check(false, "domain: 整框唯一应 execute")
        return
    }
    checkEqual(wfDomain, .wholeField, "domain: 域=wholeField")
    checkEqual(wfSelected.utf16Start, 3, "domain: 整框偏移=字段绝对偏移")

    // 注入不可定位（recent=nil）→ 整框（无记录/过期/换应用/锚定失败同型）。
    guard case .execute(let noRecentDomain, _) = VoiceTargetedDomain.resolve(
        command: plainCommand("北京"), recent: nil, fieldText: fieldUnique) else {
        check(false, "domain: 无记录应整框 execute")
        return
    }
    checkEqual(noRecentDomain, .wholeField, "domain: 无记录域=wholeField")

    // 显式作用域：recent 可用 → 严格限于 remaining；多处 → 追问（lastInjection 域）。
    guard case .clarify(let scopedCount, let scopedClarifyDomain) = VoiceTargetedDomain.resolve(
        command: plainCommand("北京", hasExplicitScope: true),
        recent: recentDouble, fieldText: field) else {
        check(false, "domain: 显式作用域多处应追问")
        return
    }
    checkEqual(scopedCount, 2, "domain: 显式作用域追问计数=域内 2")
    checkEqual(scopedClarifyDomain, .lastInjection, "domain: 显式作用域追问域=lastInjection")

    // 显式作用域 + recent 可用 + 目标不在 remaining → notFound（不回落整框）。
    guard case .failure(let scopedMiss) = VoiceTargetedDomain.resolve(
        command: plainCommand("北京", hasExplicitScope: true),
        recent: recentMiss, fieldText: fieldUnique) else {
        check(false, "domain: 显式作用域域内无目标应 notFound")
        return
    }
    checkEqual(scopedMiss, .notFound, "domain: 显式作用域 notFound")

    // 显式作用域 + recent 不可用 → 退回整框。
    guard case .execute(let scopedFallbackDomain, _) = VoiceTargetedDomain.resolve(
        command: plainCommand("北京", hasExplicitScope: true),
        recent: nil, fieldText: fieldUnique) else {
        check(false, "domain: 显式作用域不可定位应整框")
        return
    }
    checkEqual(scopedFallbackDomain, .wholeField, "domain: 显式作用域退回整框")

    // 无作用域 + 位置（第 N 个）→ 整框按文档顺序（recent 可用也不启用 Tier 1）。
    guard case .execute(let posDomain, let posSelected) = VoiceTargetedDomain.resolve(
        command: plainCommand("北京", .ordinal(2)), recent: recent, fieldText: field) else {
        check(false, "domain: 无作用域+位置应整框 execute")
        return
    }
    checkEqual(posDomain, .wholeField, "domain: 位置限定域=wholeField")
    checkEqual(posSelected.utf16Start, 7, "domain: 第 2 个=文档序第 2 处")

    // 显式作用域 + 位置 → 域内数（recent 可用）。
    guard case .execute(let scopedPosDomain, let scopedPosSelected) = VoiceTargetedDomain.resolve(
        command: plainCommand("北京", .fromEnd(1), hasExplicitScope: true),
        recent: recentDouble, fieldText: field) else {
        check(false, "domain: 显式作用域+位置应域内 execute")
        return
    }
    checkEqual(scopedPosDomain, .lastInjection, "domain: 显式作用域+位置域=lastInjection")
    checkEqual(scopedPosSelected.utf16Start, 3, "domain: 域内倒数第 1 偏移")

    // 位置越界（整框只有 3 处、要第 5 个）。
    guard case .outOfRange(let oor) = VoiceTargetedDomain.resolve(
        command: plainCommand("北京", .ordinal(5)), recent: nil, fieldText: field) else {
        check(false, "domain: 序号越界应 outOfRange")
        return
    }
    checkEqual(oor, 3, "domain: 越界携带实际计数")

    // 整框不存在目标 → notFound。
    guard case .failure(let nf) = VoiceTargetedDomain.resolve(
        command: plainCommand("广州"), recent: nil, fieldText: fieldUnique) else {
        check(false, "domain: 整框无目标应 notFound")
        return
    }
    checkEqual(nf, .notFound, "domain: 整框 notFound")

    // field_too_long：整框域 > 200,000 UTF-16（Tier 1 命中不受此限）。
    let hugeField = String(repeating: "字", count: 200_001)
    guard case .failure(let tooLong) = VoiceTargetedDomain.resolve(
        command: plainCommand("北京"), recent: nil, fieldText: hugeField) else {
        check(false, "domain: 超长字段应 fieldTooLong")
        return
    }
    checkEqual(tooLong, .fieldTooLong, "domain: field_too_long")
    // 边界：恰 200,000 不拒绝（走 notFound/正常匹配）。
    let edgeField = String(repeating: "字", count: 199_998) + "北京"
    guard case .execute(_, _) = VoiceTargetedDomain.resolve(
        command: plainCommand("北京"), recent: nil, fieldText: edgeField) else {
        check(false, "domain: 200k 边界整框唯一应 execute")
        return
    }
    // Tier 1 命中 + 超长字段 → 不受限（最近一次语音输入域沿用 2M 上限）。
    guard case .execute(let tier1LongDomain, _) = VoiceTargetedDomain.resolve(
        command: plainCommand("北京"), recent: recent, fieldText: hugeField) else {
        check(false, "domain: Tier 1 命中不受整框超长限制")
        return
    }
    checkEqual(tier1LongDomain, .lastInjection, "domain: Tier 1 超长字段仍 lastInjection")
}

// ---- 光标恢复平移三情形 + emoji ----

private func runCursorRestoreTests() {
    // 原光标 ≤ 起点 → 不变（含恰在起点）。
    checkEqual(VoiceTargetedCursorMath.restoredCaret(
        originalCaretUTF16: 2, rangeStartUTF16: 5, rangeLengthUTF16: 3, insertedLengthUTF16: 4),
        2, "cursor: 原光标在区间前不变")
    checkEqual(VoiceTargetedCursorMath.restoredCaret(
        originalCaretUTF16: 5, rangeStartUTF16: 5, rangeLengthUTF16: 3, insertedLengthUTF16: 4),
        5, "cursor: 原光标恰在起点不变")
    // 原光标 ≥ 终点 → 加长度差。
    checkEqual(VoiceTargetedCursorMath.restoredCaret(
        originalCaretUTF16: 8, rangeStartUTF16: 5, rangeLengthUTF16: 3, insertedLengthUTF16: 4),
        9, "cursor: 原光标在区间后平移（扩长）")
    checkEqual(VoiceTargetedCursorMath.restoredCaret(
        originalCaretUTF16: 10, rangeStartUTF16: 5, rangeLengthUTF16: 3, insertedLengthUTF16: 0),
        7, "cursor: 原光标在区间后平移（删除收缩）")
    checkEqual(VoiceTargetedCursorMath.restoredCaret(
        originalCaretUTF16: 8, rangeStartUTF16: 5, rangeLengthUTF16: 3, insertedLengthUTF16: 3),
        8, "cursor: 等长替换不变")
    // 原光标在区间内 → 编辑后区间末尾。
    checkEqual(VoiceTargetedCursorMath.restoredCaret(
        originalCaretUTF16: 6, rangeStartUTF16: 5, rangeLengthUTF16: 3, insertedLengthUTF16: 4),
        9, "cursor: 区间内→编辑后区间末尾（替换）")
    checkEqual(VoiceTargetedCursorMath.restoredCaret(
        originalCaretUTF16: 6, rangeStartUTF16: 5, rangeLengthUTF16: 3, insertedLengthUTF16: 0),
        5, "cursor: 区间内→编辑后区间末尾（删除=折叠点）")
    // emoji（surrogate pair）：区间 [1,3)（🙂=2 码元）删 → 后续光标 -2。
    checkEqual(VoiceTargetedCursorMath.restoredCaret(
        originalCaretUTF16: 5, rangeStartUTF16: 1, rangeLengthUTF16: 2, insertedLengthUTF16: 0),
        3, "cursor: emoji 删除平移")
    // 末尾追加情形（原光标在字段末尾=区间后）与批次 1「恢复段末」一致：
    // 字段「我去北京上班」caret=6，删 [2,4) → 6-2=4；替换等长 → 6。
    checkEqual(VoiceTargetedCursorMath.restoredCaret(
        originalCaretUTF16: 6, rangeStartUTF16: 2, rangeLengthUTF16: 2, insertedLengthUTF16: 0),
        4, "cursor: 末尾追加删除=批次 1 段末")
    checkEqual(VoiceTargetedCursorMath.restoredCaret(
        originalCaretUTF16: 6, rangeStartUTF16: 2, rangeLengthUTF16: 2, insertedLengthUTF16: 2),
        6, "cursor: 末尾追加等长替换=批次 1 段末")
}

func runVoiceTargetedDomainTests() {
    runDomainRuleTests()
    runCursorRestoreTests()
}

// ---- 序数应答解析（交付 B；与 Windows 同向量）----

private func expectReply(
    _ text: String, target: String?, _ expected: VoiceEditOrdinalReply, _ name: String
) {
    checkEqual(VoiceEditOrdinalReplyParser.parse(text, target: target), expected, name)
}

private func expectNoReply(_ text: String, target: String?, _ name: String) {
    checkNil(VoiceEditOrdinalReplyParser.parse(text, target: target), name)
}

func runVoiceEditOrdinalReplyTests() {
    // 基本形态。
    expectReply("第二个", target: "北京", .ordinal(2), "reply: 第二个")
    expectReply("第2个", target: "北京", .ordinal(2), "reply: 阿拉伯数字")
    expectReply("第十一", target: "北京", .ordinal(11), "reply: 裸第N（无数位）")
    expectReply("第十一处", target: "北京", .ordinal(11), "reply: 第N处")
    expectReply("第2处", target: "北京", .ordinal(2), "reply: 阿拉伯第N处")
    expectReply("倒数第二个", target: "北京", .fromEnd(2), "reply: 倒数第二个")
    expectReply("倒数第3个", target: "北京", .fromEnd(3), "reply: 倒数阿拉伯")
    expectReply("倒数第一处", target: "北京", .fromEnd(1), "reply: 倒数第一处")
    expectReply("最后一个", target: "北京", .fromEnd(1), "reply: 最后一个")
    expectReply("最后那个", target: "北京", .fromEnd(1), "reply: 最后那个")
    expectReply("最后一处", target: "北京", .fromEnd(1), "reply: 最后一处")
    // 带目标词（与 pending 目标一致）。
    expectReply("第二个北京", target: "北京", .ordinal(2), "reply: 第N个+目标")
    expectReply("倒数第二个北京", target: "北京", .fromEnd(2), "reply: 倒数+目标")
    expectReply("最后一个北京", target: "北京", .fromEnd(1), "reply: 最后+目标")
    expectNoReply("第二个上海", target: "北京", "reply: 目标不一致非应答")
    // 前缀（剥一层，最长优先）。
    expectReply("就第二个", target: nil, .ordinal(2), "reply: 前缀就")
    expectReply("就是第二个", target: nil, .ordinal(2), "reply: 前缀就是")
    expectReply("选择第二个", target: nil, .ordinal(2), "reply: 前缀选择")
    expectReply("选第三个", target: nil, .ordinal(3), "reply: 前缀选")
    expectReply("是第二个", target: nil, .ordinal(2), "reply: 前缀是")
    expectReply("要第二个", target: nil, .ordinal(2), "reply: 前缀要")
    expectReply("改第三个", target: nil, .ordinal(3), "reply: 前缀改")
    // 尾缀（剥一层，最长优先）+ 标点。
    expectReply("第二个吧", target: nil, .ordinal(2), "reply: 尾缀吧")
    expectReply("第二个啊", target: nil, .ordinal(2), "reply: 尾缀啊")
    expectReply("第二个呀", target: nil, .ordinal(2), "reply: 尾缀呀")
    expectReply("第二个的", target: nil, .ordinal(2), "reply: 尾缀的")
    expectReply("第二个就行", target: nil, .ordinal(2), "reply: 尾缀就行")
    expectReply("倒数第二个那个", target: nil, .fromEnd(2), "reply: 尾缀那个")
    expectReply("第二个。", target: nil, .ordinal(2), "reply: 尾部句号")
    expectReply("第二个！", target: nil, .ordinal(2), "reply: 尾部叹号")
    expectReply("就第二个吧", target: nil, .ordinal(2), "reply: 前缀+尾缀")
    expectReply("就第二个北京吧", target: "北京", .ordinal(2), "reply: 前缀+目标+尾缀")
    expectReply(" 第二个 ", target: nil, .ordinal(2), "reply: 首尾空白")
    expectReply("第２个", target: nil, .ordinal(2), "reply: 全角数字转半角")
    // 裸数字（中文一…二十、阿拉伯 1–20）。
    expectReply("五", target: nil, .ordinal(5), "reply: 裸中文五")
    expectReply("3", target: nil, .ordinal(3), "reply: 裸阿拉伯")
    expectReply("二十", target: nil, .ordinal(20), "reply: 裸二十（上限）")
    expectReply("一", target: nil, .ordinal(1), "reply: 裸一")
    expectReply("两", target: nil, .ordinal(2), "reply: 裸两")
    expectReply("就三", target: nil, .ordinal(3), "reply: 前缀+裸数字")
    expectNoReply("二十一", target: nil, "reply: 裸二十一超限")
    expectNoReply("0", target: nil, "reply: 裸 0")
    expectNoReply("零", target: nil, "reply: 裸零")
    expectNoReply("一百", target: nil, "reply: 裸一百非应答")
    // 取消类。
    expectReply("取消", target: nil, .cancel, "reply: 取消")
    expectReply("算了", target: nil, .cancel, "reply: 算了")
    expectReply("不改了", target: nil, .cancel, "reply: 不改了")
    expectReply("不用了", target: nil, .cancel, "reply: 不用了")
    expectReply("不要了", target: nil, .cancel, "reply: 不要了")
    expectReply("算了吧", target: nil, .cancel, "reply: 取消+尾缀")
    expectReply("就取消", target: nil, .cancel, "reply: 取消+前缀")
    // 非应答（放行正常链路）。
    expectNoReply("你好", target: nil, "reply: 普通文本非应答")
    expectNoReply("把北京改成上海", target: nil, "reply: 完整指令非应答")
    expectNoReply("删除刚才的北京", target: nil, "reply: 删除指令非应答")
    expectNoReply("第二个北京怎么样", target: "北京", "reply: 目标后夹带非应答")
    expectNoReply("", target: nil, "reply: 空串")
}
