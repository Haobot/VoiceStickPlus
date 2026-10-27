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
    // 任一侧为空（编辑追踪纯追加/纯删除的中段形态）：返回空对，且不触发
    // 1...n 空区间崩溃（回归：VoiceStickApp 2026-10-15 崩溃）。
    check(
        CorrectionPairs.extractReplacementPairs("", "啊").isEmpty,
        "diff 提取：空 original 返回空"
    )
    check(
        CorrectionPairs.extractReplacementPairs("你好世界", "").isEmpty,
        "diff 提取：空 refined 返回空"
    )
    check(
        CorrectionPairs.extractReplacementPairs("", "").isEmpty,
        "diff 提取：双侧为空返回空"
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

    // ---- App 场景分域（wetype P1-2 / doubao §3.4）：同对不同桶各自累计；查询桶优先 ----
    var bucketStore: [CorrectionPair] = []
    let bucketPairs = [
        CorrectionReplacement(before: "克劳德", after: "Claude"),
        CorrectionReplacement(before: "魏碑", after: "WeBank")
    ]
    CorrectionPairs.record(&bucketStore, pairs: bucketPairs, nowTs: 1000, app: "com.microsoft.VSCode")
    CorrectionPairs.record(&bucketStore, pairs: bucketPairs, nowTs: 2000, app: "")
    CorrectionPairs.record(&bucketStore, pairs: bucketPairs, nowTs: 3000, app: "com.microsoft.VSCode")
    // 对齐 doubao UNIQUE(src,dst,app_bucket)：同对在两个桶各自累计 = 4 条。
    checkEqual(bucketStore.count, 4, "分域：同对不同桶各自累计")
    checkEqual(bucketStore[0].app, "com.microsoft.VSCode", "分域：App 桶标记")
    checkEqual(bucketStore[0].count, 2, "分域：App 桶计数独立")
    checkEqual(bucketStore[2].count, 1, "分域：全局桶计数独立")

    // 精确命中：App 桶优先，miss 落全局桶。
    var mixed: [CorrectionPair] = []
    mixed.append(CorrectionPair(before: "部署到", after: "部署到服务器", count: 9,
                                app: "com.microsoft.VSCode"))
    mixed.append(CorrectionPair(before: "部署到", after: "部署到集群", count: 5, app: ""))
    checkEqual(
        CorrectionPairs.exactReplacement(for: "部署到", in: mixed, app: "com.microsoft.VSCode")?.after,
        "部署到服务器",
        "分域：精确命中 App 桶优先"
    )
    checkEqual(
        CorrectionPairs.exactReplacement(for: "部署到", in: mixed, app: "com.apple.Safari")?.after,
        "部署到集群",
        "分域：无当前桶命中落全局桶"
    )
    // 其他 App 的桶不参与查询。
    checkEqual(
        CorrectionPairs.trimForPrompt(mixed, maxPairs: 8, app: "com.apple.Safari").count,
        1,
        "分域：其他 App 桶被排除"
    )

    // few-shot 查询：App 桶优先占预算；其他 App 的桶不参与、全局桶兜底。
    let bucketRanked = CorrectionPairs.trimForPrompt(mixed, maxPairs: 1, app: "com.microsoft.VSCode")
    checkEqual(bucketRanked.first?.after, "部署到服务器", "分域：few-shot App 桶优先占预算")
    let globalRanked = CorrectionPairs.trimForPrompt(mixed, maxPairs: 2, app: "com.apple.Safari")
    checkEqual(globalRanked.map(\.after), ["部署到集群"], "分域：无当前桶时只落全局桶")

    // ---- P3 反馈环：recall_count + 精确率降权（doubao §3.3）----
    let recallURL = FileManager.default.temporaryDirectory
        .appendingPathComponent("voicestick_recall_loop.json")
    try? FileManager.default.removeItem(at: recallURL)
    CorrectionPairs.recordToDisk(
        url: recallURL,
        pairs: [CorrectionReplacement(before: "鱼器渍", after: "语气词")],
        nowTs: 1000
    )
    let recalledPair = CorrectionPairs.load(url: recallURL).first!
    CorrectionPairs.markRecalledOnDisk(url: recallURL, pair: recalledPair)
    CorrectionPairs.markRecalledOnDisk(url: recallURL, pair: recalledPair)
    let recalled = CorrectionPairs.load(url: recallURL)
    checkEqual(recalled.first?.recallCount, 2, "反馈环：确认保留 recall+1 落盘")
    checkEqual(recalled.first?.useCount, 0, "反馈环：recall 不动 use")

    // 降权判定：use ≥10 且 recall/use < 0.5。
    let demoted = CorrectionPair(before: "坏对", after: "wrong", count: 1, useCount: 10, recallCount: 4)
    let healthy = CorrectionPair(before: "好对", after: "right", count: 1, useCount: 10, recallCount: 8)
    let young = CorrectionPair(before: "新对", after: "new", count: 1, useCount: 3, recallCount: 0)
    check(demoted.isDemoted, "降权：use≥10 且精确率 <0.5")
    check(!healthy.isDemoted, "降权：精确率充足不降权")
    check(!young.isDemoted, "降权：应用不足 10 次不降权（观察期）")

    // 精确命中：降权对从自动应用池摘除。
    let demoteStore = [demoted, healthy, young]
    checkEqual(
        CorrectionPairs.exactReplacement(for: "坏对", in: demoteStore)?.after,
        nil,
        "降权：降权对不再自动应用"
    )
    checkEqual(
        CorrectionPairs.exactReplacement(for: "好对", in: demoteStore)?.after,
        "right",
        "降权：健康对正常自动应用"
    )
    // few-shot：降权对沉底。
    let sinkRanked = CorrectionPairs.trimForPrompt(demoteStore, maxPairs: 8)
    checkEqual(sinkRanked.last?.before, "坏对", "降权：few-shot 降权对沉底")
    checkEqual(sinkRanked.first?.before, "好对", "降权：观察期对正常排序（字典序）")

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
