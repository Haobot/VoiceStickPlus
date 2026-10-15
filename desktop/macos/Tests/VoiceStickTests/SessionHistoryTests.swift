import Foundation
import VoiceStickCore

// 会话历史数据底座单测（用户数据飞轮 P0-3，Typeless 方案 P0-1 history_v2 裁剪版）：
// 库打开/迁移幂等、插入读回、injected 回填、损坏路径降级。

func runSessionHistoryTests() {
    let dir = FileManager.default.temporaryDirectory
        .appendingPathComponent("voicestick_session_history_tests")
    try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    let dbURL = dir.appendingPathComponent("session_history.db")
    try? FileManager.default.removeItem(at: dbURL)

    // 打开 + 迁移 + 插入读回。
    guard let store = unwrap(SessionHistoryStore(url: dbURL), "history：正常路径打开成功") else { return }
    checkEqual(store.rowCount(), 0, "history：空库行数 0")

    var first = SessionHistoryRecord(
        id: "session-1", createdAt: 1_760_000_000, deviceID: "AB12",
        asrProvider: "volcengine", mode: "original", rawAsr: "这个鱼器渍是什么",
        refinedText: "这个语气词是什么", injectedText: "这个语气词是什么",
        duration: 3.2,
        hotwordsSent: "[\"语气词\"]"
    )
    first.clientMetadata = "{\"app_version\":\"2.4.10\"}"
    store.insert(first)
    let second = SessionHistoryRecord(id: "session-2", createdAt: 1_760_000_100)
    store.insert(second)
    checkEqual(store.rowCount(), 2, "history：插入两行")

    // 省略字段走默认值（edited_* 三件套预留编辑追踪分支接入）。
    checkEqual(second.editedTextStatus, kSessionHistoryEditedStatusDefault, "history：edited 状态默认值")
    checkEqual(second.editedTextAttempts, 0, "history：edited 尝试数默认 0")

    // 重开同库：迁移幂等，存量资产行数不丢（schema 演进不破坏存量）。
    store.close()
    guard let reopened = unwrap(SessionHistoryStore(url: dbURL), "history：重开成功（迁移幂等）") else { return }
    checkEqual(reopened.rowCount(), 2, "history：重开后行数保留")
    checkEqual(
        reopened.latestRefinedTexts(limit: 5),
        ["这个语气词是什么"],
        "history：latestRefinedTexts 过滤空 refined 且旧→新排序"
    )

    // translate 注入文本回填。
    reopened.updateInjectedText(id: "session-2", text: "translated text")
    checkEqual(reopened.rowCount(), 2, "history：回填不新增行")

    // 目录路径打不开 → init 返回 nil（降级停用，不抛错）。
    checkNil(SessionHistoryStore(url: dir), "history：损坏路径打开返回 nil（降级停用）")

    try? FileManager.default.removeItem(at: dir)
}
