import Foundation
import VoiceStickCore

// 热词候选挖掘单测（用户数据飞轮 P0-2，自 Windows hotword_candidate_miner 移植语义；
// 与 desktop/windows/tests/core_tests_license_ima.cc 的 TestHotwordCandidateMiner /
// TestHotwordCandidatesSingleWriter 同向量钉死）。

func runHotwordCandidateMinerTests() {
    // ---- 标识符提取：字母开头 + 含大写/数字/._-；普通小写词、版本号、CJK 不算；
    // 尾部标点剥离。 ----
    checkEqual(
        HotwordCandidateMiner.extractIdentifierTokens(
            "编辑 AGENTS.md 和 hello world 文件，version 2.0 发布了 RamDisk、build_win.bat。"
        ),
        ["AGENTS.md", "RamDisk", "build_win.bat"],
        "标识符提取：混合文本"
    )
    check(
        HotwordCandidateMiner.extractIdentifierTokens("plain english words only").isEmpty,
        "标识符提取：普通小写英文词不算"
    )
    checkEqual(
        HotwordCandidateMiner.extractIdentifierTokens("kAutoHideTimerId."),
        ["kAutoHideTimerId"],
        "标识符提取：尾部句号剥离"
    )

    // ---- 挖掘：refined 新增且不在热词表的标识符。 ----
    checkEqual(
        HotwordCandidateMiner.mineRefinementCandidates(
            original: "测试 agentsdmd 和 cloud dmd 文件",
            refined: "测试 AGENTS.md 和 CLAUDE.md 文件。",
            existingHotwords: []
        ),
        ["AGENTS.md", "CLAUDE.md"],
        "挖掘：refined 新增标识符"
    )
    check(
        HotwordCandidateMiner.mineRefinementCandidates(
            original: "测试 agentsdmd 文件",
            refined: "测试 AGENTS.md 文件。",
            existingHotwords: ["AGENTS.md"]
        ).isEmpty,
        "挖掘：已入热词表的不重复挖"
    )
    check(
        HotwordCandidateMiner.mineRefinementCandidates(
            original: "测试 RamDisk 文件",
            refined: "测试 RamDisk 文件。",
            existingHotwords: []
        ).isEmpty,
        "挖掘：原文已有的词不挖"
    )

    // ---- 计数到阈值才建议；忽略词不建议；已通知词不重复建议。 ----
    var store = HotwordCandidateStore()
    check(HotwordCandidateMiner.record(&store, words: ["AGENTS.md"]).isEmpty, "阈值：第 1 次不建议")
    check(HotwordCandidateMiner.record(&store, words: ["AGENTS.md"]).isEmpty, "阈值：第 2 次不建议")
    let suggested = HotwordCandidateMiner.record(&store, words: ["AGENTS.md", "CLAUDE.md"])
    checkEqual(suggested, ["AGENTS.md"], "阈值：第 3 次建议（另一词仅 1 次）")
    checkEqual(store.counts["AGENTS.md"], 3, "计数：达阈值词累计 3")
    checkEqual(store.counts["CLAUDE.md"], 1, "计数：新词累计 1")
    store.notified.insert("AGENTS.md")
    check(HotwordCandidateMiner.record(&store, words: ["AGENTS.md"]).isEmpty, "已通知词不重复建议")
    store.dismissed.insert("CLAUDE.md")
    check(HotwordCandidateMiner.record(&store, words: ["CLAUDE.md"]).isEmpty, "忽略词不建议")

    // ---- 待确认列表：达阈值且未忽略（字典序，对齐 Windows std::map 遍历序）。 ----
    checkEqual(HotwordCandidateMiner.pendingSuggestions(store), ["AGENTS.md"], "待确认：达阈值且未忽略")
    store.dismissed.insert("AGENTS.md")
    check(HotwordCandidateMiner.pendingSuggestions(store).isEmpty, "待确认：忽略后清空")

    // ---- JSON 保存/加载往返（与 Windows hotword_candidates.json 同构）。 ----
    let temp = FileManager.default.temporaryDirectory
        .appendingPathComponent("voicestick_hotword_candidates_test.json")
    try? FileManager.default.removeItem(at: temp)
    var roundtrip = HotwordCandidateStore()
    roundtrip.counts["AGENTS.md"] = 5
    roundtrip.dismissed.insert("cloud dmd")
    roundtrip.notified.insert("VoiceStick")
    HotwordCandidateMiner.save(roundtrip, url: temp)
    let loaded = HotwordCandidateMiner.load(url: temp)
    checkEqual(loaded.counts, ["AGENTS.md": 5], "JSON 往返：counts")
    checkEqual(loaded.dismissed, ["cloud dmd"], "JSON 往返：dismissed")
    checkEqual(loaded.notified, ["VoiceStick"], "JSON 往返：notified")
    try? FileManager.default.removeItem(at: temp)
    check(HotwordCandidateMiner.load(url: temp).counts.isEmpty, "JSON：缺失文件返回空 store")

    // ---- B13 单写入口：recordToDisk reload-merge-save，忽略词不回弹（Windows
    // TestHotwordCandidatesSingleWriter 同款回归）。 ----
    let dir = FileManager.default.temporaryDirectory
        .appendingPathComponent("voicestick_b13_candidates_swift")
    try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    let path = dir.appendingPathComponent("hotword_candidates.json")
    try? FileManager.default.removeItem(at: path)

    // 挖掘三轮达阈值（kHotwordCandidateThreshold = 3）→ 第三轮给出建议并记入 notified。
    check(HotwordCandidateMiner.recordToDisk(url: path, words: ["AlphaCode"]).isEmpty, "单写者：第 1 轮无建议")
    check(HotwordCandidateMiner.recordToDisk(url: path, words: ["AlphaCode"]).isEmpty, "单写者：第 2 轮无建议")
    checkEqual(
        HotwordCandidateMiner.recordToDisk(url: path, words: ["AlphaCode"]),
        ["AlphaCode"],
        "单写者：第 3 轮给出建议"
    )
    checkEqual(
        HotwordCandidateMiner.pendingSuggestions(HotwordCandidateMiner.load(url: path)),
        ["AlphaCode"],
        "单写者：建议仍待确认"
    )

    // 用户在设置页「忽略」。
    check(HotwordCandidateMiner.dismissOnDisk(url: path, word: "AlphaCode"), "单写者：忽略返回真")
    check(
        HotwordCandidateMiner.pendingSuggestions(HotwordCandidateMiner.load(url: path)).isEmpty,
        "单写者：忽略后待确认清空"
    )
    // 关键回归：后续挖掘不得让已忽略的词重新弹回（Windows load-once 缓存旧病）。
    let after = HotwordCandidateMiner.recordToDisk(url: path, words: ["AlphaCode", "FreshWord"])
    check(!after.contains("AlphaCode"), "单写者：忽略词不回弹")
    check(
        HotwordCandidateMiner.load(url: path).dismissed.contains("AlphaCode"),
        "单写者：忽略标记持久保留"
    )
    // 「加入」消费路径：从 counts/notified 移除。
    HotwordCandidateMiner.recordToDisk(url: path, words: ["FreshWord"])
    check(HotwordCandidateMiner.consumeOnDisk(url: path, word: "FreshWord"), "单写者：加入返回真")
    check(
        HotwordCandidateMiner.load(url: path).counts["FreshWord"] == nil,
        "单写者：加入后从存储移除"
    )

    // 写入原子性：保存后文件可立即解析。
    for _ in 0..<3 { HotwordCandidateMiner.recordToDisk(url: path, words: ["AtomicCheck"]) }
    check(
        HotwordCandidateMiner.load(url: path).counts["AtomicCheck"] != nil,
        "单写者：写入后立即可解析"
    )
    try? FileManager.default.removeItem(at: dir)
}
