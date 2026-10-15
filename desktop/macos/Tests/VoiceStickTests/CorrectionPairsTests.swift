import Foundation
import VoiceStickCore

// 纠错对资产化单测（用户数据飞轮 P0-1，自 Windows correction_pairs 移植语义；
// 与 desktop/windows/tests/core_tests_corrections.cc 同向量钉死——prompt 段
// 构造、diff 提取、存储合并/容量逐出、JSON 往返双端一致）。

func runCorrectionPairsTests() {
    // ---- extractReplacementPairs ----
    // 基准用例：同音错词「鱼器渍→语气词」。
    checkEqual(
        CorrectionPairs.extractReplacementPairs("这个鱼器渍是什么", "这个语气词是什么"),
        [CorrectionReplacement(before: "鱼器渍", after: "语气词")],
        "diff 提取：同音错词基准用例"
    )
    // 删除/插入（单侧为空）不入表。
    check(
        CorrectionPairs.extractReplacementPairs("我觉得嗯这个方案可以", "我觉得这个方案可以").isEmpty,
        "diff 提取：删除片段不入表"
    )
    // 完全一致无对。
    check(
        CorrectionPairs.extractReplacementPairs("你好世界", "你好世界").isEmpty,
        "diff 提取：完全一致无对"
    )
    // 拉丁词纠正（整词无公共子序列干扰，对形态完整）。
    checkEqual(
        CorrectionPairs.extractReplacementPairs("我用克劳德写代码", "我用 Claude 写代码"),
        [CorrectionReplacement(before: "克劳德", after: "Claude")],
        "diff 提取：拉丁词纠正保留"
    )
    // 纯标点/空白调整不入表（无内容字符锚点）。
    check(
        CorrectionPairs.extractReplacementPairs("好的，谢谢", "好的、谢谢").isEmpty,
        "diff 提取：纯标点调整不入表"
    )
    check(
        CorrectionPairs.extractReplacementPairs("中文  之间空格", "中文之间空格").isEmpty,
        "diff 提取：纯空白调整不入表"
    )
    // 多对同时提取，保序去重。
    checkEqual(
        CorrectionPairs.extractReplacementPairs("用甲和乙测试", "用 A 和 B 测试"),
        [CorrectionReplacement(before: "甲", after: "A"),
         CorrectionReplacement(before: "乙", after: "B")],
        "diff 提取：多对保序"
    )
    // 单侧超过 kCorrectionPairMaxSideChars(24) 码点：整句改写量级不入表。
    let longRun = String(repeating: "x", count: 25)
    check(
        CorrectionPairs.extractReplacementPairs("前缀\(longRun)后缀", "前缀Y后缀").isEmpty,
        "diff 提取：超长侧不入表"
    )
    // 任一侧超过 512 码点（整句改写量级护栏）返回空。
    let huge = String(repeating: "a", count: 600)
    check(
        CorrectionPairs.extractReplacementPairs(huge, huge + "b").isEmpty,
        "diff 提取：超 512 码点护栏"
    )

    // ---- record：合并计数 + 容量逐出 ----
    var store: [CorrectionPair] = []
    CorrectionPairs.record(&store, pairs: [CorrectionReplacement(before: "鱼器渍", after: "语气词")], nowTs: 1000)
    CorrectionPairs.record(
        &store,
        pairs: [CorrectionReplacement(before: "鱼器渍", after: "语气词"),
                CorrectionReplacement(before: "克劳德", after: "Claude")],
        nowTs: 2000
    )
    checkEqual(store.count, 2, "record：去重合并为两条")
    checkEqual(store[0].count, 2, "record：同对计数累计")
    checkEqual(store[0].lastSeenTs, Int64(2000), "record：刷新最近时间戳")
    checkEqual(store[1].count, 1, "record：新对计数 1")

    // 容量硬上限：超限按 count 升序、last_seen 升序逐出，保留 1000 条。
    var big: [CorrectionPair] = []
    let flood = (0..<(kCorrectionPairStoreCap + 5)).map { index -> CorrectionReplacement in
        CorrectionReplacement(before: "词\(index)", after: "word\(index)")
    }
    CorrectionPairs.record(&big, pairs: flood, nowTs: 3000)
    checkEqual(big.count, kCorrectionPairStoreCap, "record：容量封顶")
    check(
        !big.contains { $0.before == "词0" },
        "record：字典序靠前的新对先逐出"
    )
    check(
        big.contains { $0.before == "词\(kCorrectionPairStoreCap + 4)" },
        "record：最后一个对保留"
    )

    // ---- JSON 往返 + 损坏容错 ----
    let temp = FileManager.default.temporaryDirectory
        .appendingPathComponent("voicestick_correction_pairs_test.json")
    try? FileManager.default.removeItem(at: temp)
    let roundtrip = [CorrectionPair(before: "鱼器渍", after: "语气词", count: 3, lastSeenTs: 1_760_000_000)]
    CorrectionPairs.save(roundtrip, url: temp)
    let loaded = CorrectionPairs.load(url: temp)
    checkEqual(loaded, roundtrip, "JSON：保存/加载往返一致")
    try? FileManager.default.removeItem(at: temp)
    check(CorrectionPairs.load(url: temp).isEmpty, "JSON：缺失文件返回空 store")
    try? "{broken json".data(using: .utf8)!.write(to: temp)
    check(CorrectionPairs.load(url: temp).isEmpty, "JSON：损坏文件返回空 store")
    try? FileManager.default.removeItem(at: temp)

    // ---- trimForPrompt：排序与截断 ----
    var ranked: [CorrectionPair] = []
    ranked.append(CorrectionPair(before: "低频", after: "low", count: 1, lastSeenTs: 100))
    ranked.append(CorrectionPair(before: "高频旧", after: "old", count: 5, lastSeenTs: 100))
    ranked.append(CorrectionPair(before: "高频新", after: "new", count: 5, lastSeenTs: 900))
    ranked.append(CorrectionPair(before: "零次", after: "zero", count: 0, lastSeenTs: 999))
    let trimmed = CorrectionPairs.trimForPrompt(ranked, maxPairs: 8)
    checkEqual(trimmed.map(\.before), ["高频新", "高频旧", "低频"], "trim：count 降序 + 新近度次序，count<=0 剔除")
    checkEqual(CorrectionPairs.trimForPrompt(ranked, maxPairs: 2).count, 2, "trim：maxPairs 截断")
    check(CorrectionPairs.trimForPrompt(ranked, maxPairs: 0).isEmpty, "trim：非正值返回空")

    // ---- promptSection：两端逐字一致 ----
    check(CorrectionPairs.promptSection([]).isEmpty, "few-shot：空集返回空串")
    let pairs = [
        CorrectionPair(before: "鱼器渍", after: "语气词"),
        CorrectionPair(before: "克劳德", after: "Claude")
    ]
    let expected =
        "\n\n用户历史纠正（输出时请遵循这些偏好：仅当识别文本出现与纠正前写法相同或同音、"
        + "近音的错误时替换为纠正后写法，其余内容不得改动）："
        + "\n- \"鱼器渍\" → \"语气词\""
        + "\n- \"克劳德\" → \"Claude\""
    checkEqual(CorrectionPairs.promptSection(pairs), expected, "few-shot：段文本逐字一致")
    let quotePair = CorrectionPair(before: "带\"引号", after: "ok")
    let newlinePair = CorrectionPair(before: "带\n换行", after: "ok")
    check(
        CorrectionPairs.promptSection([quotePair, newlinePair]).isEmpty,
        "few-shot：含引号/换行的对全部过滤后返回空串"
    )
    let expectedFirstOnly =
        "\n\n用户历史纠正（输出时请遵循这些偏好：仅当识别文本出现与纠正前写法相同或同音、"
        + "近音的错误时替换为纠正后写法，其余内容不得改动）："
        + "\n- \"鱼器渍\" → \"语气词\""
    checkEqual(
        CorrectionPairs.promptSection([pairs[0], quotePair]),
        expectedFirstOnly,
        "few-shot：无效对被逐条跳过"
    )

    // ---- exactReplacement：全句精确命中（doubao 方案 §3.1 应用位置 1）----
    var applyStore: [CorrectionPair] = []
    applyStore.append(CorrectionPair(before: "鱼器渍", after: "语气词", count: 1))
    applyStore.append(CorrectionPair(before: "克劳德", after: "Claude", count: 3))
    applyStore.append(CorrectionPair(before: "零次", after: "zero", count: 0))
    checkEqual(
        CorrectionPairs.exactReplacement(for: "克劳德", in: applyStore)?.after,
        "Claude",
        "精确命中：多条命中取 count 最高"
    )
    checkEqual(
        CorrectionPairs.exactReplacement(for: " 鱼器渍 ", in: applyStore)?.after,
        "语气词",
        "精确命中：首尾空白容忍"
    )
    checkNil(
        CorrectionPairs.exactReplacement(for: "这个鱼器渍是什么", in: applyStore),
        "精确命中：子串不算（只做整句替换）"
    )
    checkNil(
        CorrectionPairs.exactReplacement(for: "零次", in: applyStore),
        "精确命中：count<=0 剔除"
    )
    checkNil(CorrectionPairs.exactReplacement(for: "", in: applyStore), "精确命中：空文本不命中")

    // markUsedOnDisk：use_count 累计落盘。
    let applyURL = FileManager.default.temporaryDirectory
        .appendingPathComponent("voicestick_correction_apply.json")
    try? FileManager.default.removeItem(at: applyURL)
    CorrectionPairs.recordToDisk(
        url: applyURL,
        pairs: [CorrectionReplacement(before: "鱼器渍", after: "语气词")],
        nowTs: 1000
    )
    let storedPair = CorrectionPairs.load(url: applyURL).first!
    CorrectionPairs.markUsedOnDisk(url: applyURL, pair: storedPair, nowTs: 2000)
    CorrectionPairs.markUsedOnDisk(url: applyURL, pair: storedPair, nowTs: 3000)
    let applied = CorrectionPairs.load(url: applyURL)
    checkEqual(applied.count, 1, "markUsed：不新增行")
    checkEqual(applied.first?.useCount, 2, "markUsed：use_count 累计 2")
    checkEqual(applied.first?.lastSeenTs, Int64(3000), "markUsed：刷新时间戳")
    try? FileManager.default.removeItem(at: applyURL)

    // ---- recordToDisk：B13 reload-merge-save 跨调用累计 ----
    let diskURL = FileManager.default.temporaryDirectory
        .appendingPathComponent("voicestick_correction_pairs_rw.json")
    try? FileManager.default.removeItem(at: diskURL)
    CorrectionPairs.recordToDisk(
        url: diskURL, pairs: [CorrectionReplacement(before: "鱼器渍", after: "语气词")], nowTs: 1000
    )
    CorrectionPairs.recordToDisk(
        url: diskURL, pairs: [CorrectionReplacement(before: "鱼器渍", after: "语气词")], nowTs: 2000
    )
    let diskStore = CorrectionPairs.load(url: diskURL)
    checkEqual(diskStore.count, 1, "recordToDisk：跨调用合并不覆盖")
    checkEqual(diskStore.first?.count, 2, "recordToDisk：计数累计为 2")
    // 空提取不触碰文件。
    CorrectionPairs.recordToDisk(url: diskURL, pairs: [], nowTs: 3000)
    check(FileManager.default.fileExists(atPath: diskURL.path), "recordToDisk：空提取不落空文件")
    try? FileManager.default.removeItem(at: diskURL)
}
