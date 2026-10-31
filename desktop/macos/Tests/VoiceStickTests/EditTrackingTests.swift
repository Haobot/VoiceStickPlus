import Foundation
import VoiceStickCore

// 编辑追踪纯逻辑单测（用户数据飞轮 P0-2，Typeless 方案：改动区间口径 + 隐私红线
// 「只取改动 span，不落全字段」）。

func runEditTrackingTests() {
    // 未编辑：字段与注入一致 → nil。
    checkNil(
        EditTracking.editedRegion(fieldText: "你好世界", injectedText: "你好世界"),
        "editRegion：未编辑返回 nil"
    )
    // 焦点字段与注入公共前后缀过低（焦点切走/整体重写）→ nil（无法归因）。
    checkNil(
        EditTracking.editedRegion(fieldText: "完全无关的内容", injectedText: "你好世界"),
        "editRegion：无关字段不作归因"
    )
    checkNil(
        EditTracking.editedRegion(fieldText: "", injectedText: "你好世界"),
        "editRegion：空字段返回 nil"
    )

    // 中段改写：「语气词」→「鱼器渍」——注入侧/字段侧中段成对返回。
    let rewritten = EditTracking.editedRegion(
        fieldText: "这个鱼器渍是什么", injectedText: "这个语气词是什么"
    )
    checkEqual(rewritten?.before, "语气词", "editRegion：中段改写 before 侧")
    checkEqual(rewritten?.after, "鱼器渍", "editRegion：中段改写 after 侧")
    // 末尾追加：注入侧中段为空串、字段侧为追加内容。
    let appended = EditTracking.editedRegion(fieldText: "你好世界啊", injectedText: "你好世界")
    checkEqual(appended?.before, "", "editRegion：追加 before 为空")
    checkEqual(appended?.after, "啊", "editRegion：追加 after")
    // 中段删除（字段侧中段为空串）。
    let deleted = EditTracking.editedRegion(
        fieldText: "我觉得这个方案可以", injectedText: "我觉得嗯这个方案可以"
    )
    checkEqual(deleted?.before, "嗯", "editRegion：删除 before")
    checkEqual(deleted?.after, "", "editRegion：纯删除 after 为空")

    // 纯追加编辑整链路（回归：VoiceStickApp 2026-10-15 崩溃——before 为空串时
    // extractReplacementPairs 的 1...n 空区间崩溃）：追加 region 必须产出空对。
    let appendRegion = EditTracking.editedRegion(
        fieldText: "你好世界啊", injectedText: "你好世界"
    )
    checkEqual(
        CorrectionPairs.extractReplacementPairs(appendRegion?.before ?? "x", appendRegion?.after ?? "x"),
        [],
        "editRegion→extract：纯追加编辑返回空对不崩溃"
    )

    // recordToDisk source=edit：来源字段写入 JSON 且读回一致（纠错对来源可观测）。
    let url = FileManager.default.temporaryDirectory
        .appendingPathComponent("voicestick_correction_source_edit.json")
    try? FileManager.default.removeItem(at: url)
    CorrectionPairs.recordToDisk(
        url: url,
        pairs: [CorrectionReplacement(before: "鱼器渍", after: "语气词")],
        nowTs: 1000,
        source: "edit"
    )
    checkEqual(
        CorrectionPairs.load(url: url).first?.source,
        "edit",
        "record：source=edit 落库"
    )
    try? FileManager.default.removeItem(at: url)

    // SessionHistory.updateEditedText：edited_* 三件套回填。
    let dir = FileManager.default.temporaryDirectory
        .appendingPathComponent("voicestick_edit_tracking_db")
    try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    let dbURL = dir.appendingPathComponent("session_history.db")
    try? FileManager.default.removeItem(at: dbURL)
    guard let store = unwrap(SessionHistoryStore(url: dbURL), "editTracking：库打开") else { return }
    store.insert(SessionHistoryRecord(id: "s1", createdAt: 1, refinedText: "你好世界"))
    store.updateEditedText(
        id: "s1", text: "啊", status: EditTrackingStatus.extracted, attempts: 2
    )
    checkEqual(store.rowCount(), 1, "editTracking：回填不新增行")
    let edited = store.editedText(id: "s1")
    checkEqual(edited?.text, "啊", "editTracking：edited span 读回")
    checkEqual(edited?.status, EditTrackingStatus.extracted, "editTracking：状态读回")
    checkEqual(edited?.attempts, 2, "editTracking：尝试数读回")
    checkNil(store.editedText(id: "missing"), "editTracking：未知行返回 nil")
    store.close()
}
