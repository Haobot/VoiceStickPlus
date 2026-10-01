import Foundation

// 热词评分与预算裁剪（自 Windows hotword_selector.{h,cc} 1:1 移植；对应
// scripts/e2e_test/asr_bench/hotword_select.py 与
// Doc/Plan/hotword-eval-and-prioritization.md §3）。
//
// 背景：热词库持续增长，直传预算有限（火山 corpus 80 tokens / LLM prompt 注意力）。
// 超预算时按 score = 1.0·log1p(count) + 0.5·exp(-age/30d) + 2.0·manual
// 优先保留高频/新近/手动词，替代按插入顺序贪心截断（新词排尾部最先被裁）。

/// 评分权重（与 hotword_select.py 保持一致）。
public let kHotwordWCount = 1.0
public let kHotwordWRecency = 0.5
public let kHotwordWManual = 2.0
public let kHotwordRecencyTauS: Int64 = 30 * 24 * 3600

/// 精修/翻译 prompt 热词段上限（防大库稀释小模型注意力）。
public let kHotwordPromptMaxWords = 50

public struct HotwordUsage: Equatable {
    public var count: Int = 0                 // 出现在最终文本中的累计次数
    public var lastUsedTs: Int64 = 0          // 最近使用 epoch 秒（0 = 未知，按最旧处理）
    public var source: String = "mined"       // "manual" / "mined"

    public init(count: Int = 0, lastUsedTs: Int64 = 0, source: String = "mined") {
        self.count = count
        self.lastUsedTs = lastUsedTs
        self.source = source
    }
}

public typealias HotwordUsageStore = [String: HotwordUsage]

public enum HotwordSelector {
    /// 硬约束（与 hotword_select.py 的 is_valid_word 一致）：不含空白，
    /// ≤10 汉字（非 ASCII 码点）/ ≤30 英文字符。
    public static func isValidHotword(_ word: String) -> Bool {
        if word.isEmpty { return false }
        var cjk = 0
        var ascii = 0
        for scalar in word.unicodeScalars {
            if !scalar.isASCII {
                cjk += 1
            } else {
                if CharacterSet.whitespacesAndNewlines.contains(scalar) { return false }
                ascii += 1
            }
        }
        return cjk <= 10 && ascii <= 30
    }

    /// 评分；last_used_ts 未知（0）时新近度记 0。
    public static func score(_ usage: HotwordUsage, nowS: Int64) -> Double {
        let countTerm = kHotwordWCount * log1p(Double(max(0, usage.count)))
        var recency = 0.0
        if usage.lastUsedTs > 0 {
            let age = max(0.0, Double(nowS - usage.lastUsedTs))
            recency = kHotwordWRecency * exp(-age / Double(kHotwordRecencyTauS))
        }
        let manual = usage.source == "manual" ? kHotwordWManual : 0.0
        return countTerm + recency + manual
    }

    /// 按评分降序返回；缺统计记录的词按 manual 处理（现状所有热词均由用户动作入表，
    /// 避免新加的词最先被裁）；非法词过滤；同分按字典序保证确定性。
    public static func rank(
        _ store: HotwordUsageStore, hotwords: [String], nowS: Int64
    ) -> [String] {
        var entries: [(word: String, score: Double)] = []
        entries.reserveCapacity(hotwords.count)
        for word in hotwords where isValidHotword(word) {
            let usage = store[word] ?? HotwordUsage(source: "manual")
            entries.append((word, score(usage, nowS: nowS)))
        }
        entries.sort { lhs, rhs in
            if lhs.score != rhs.score { return lhs.score > rhs.score }
            return lhs.word < rhs.word
        }
        return entries.map(\.word)
    }

    /// 取评分最高的前 maxWords 个（精修/翻译 prompt 热词段）。
    public static func trimForPrompt(
        _ store: HotwordUsageStore, hotwords: [String], maxWords: Int, nowS: Int64
    ) -> [String] {
        let ranked = rank(store, hotwords: hotwords, nowS: nowS)
        if maxWords <= 0 { return [] }
        if ranked.count <= maxWords { return ranked }
        return Array(ranked.prefix(maxWords))
    }

    /// 记录文本中出现的热词：计数 + 刷新最近使用时间戳（大小写不敏感子串匹配）。
    /// 只更新内存 store，落盘由调用方负责。
    public static func recordUsage(
        _ store: inout HotwordUsageStore, text: String, hotwords: [String], nowS: Int64
    ) {
        let lowered = text.lowercased()
        for word in hotwords where !word.isEmpty && lowered.contains(word.lowercased()) {
            var usage = store[word] ?? HotwordUsage(source: "manual")
            usage.count += 1
            usage.lastUsedTs = nowS
            store[word] = usage
        }
    }

    /// JSON 读取（数组形态 [{word,count,last_used_ts,source}, ...]，与
    /// hotword_select.py 的 load_stats_json 兼容）；缺失/损坏返回空 store。
    public static func loadUsage(url: URL) -> HotwordUsageStore {
        guard let data = try? Data(contentsOf: url),
              let array = try? JSONSerialization.jsonObject(with: data) as? [[String: Any]] else {
            return [:]
        }
        var store: HotwordUsageStore = [:]
        for item in array {
            guard let word = item["word"] as? String else { continue }
            var usage = HotwordUsage()
            if let count = item["count"] as? Int { usage.count = count }
            if let ts = item["last_used_ts"] as? Int64 { usage.lastUsedTs = ts }
            if let ts = item["last_used_ts"] as? Int { usage.lastUsedTs = Int64(ts) }
            if let source = item["source"] as? String, !source.isEmpty { usage.source = source }
            store[word] = usage
        }
        return store
    }

    /// JSON 写入（best-effort，写失败静默）。
    public static func saveUsage(_ store: HotwordUsageStore, url: URL) {
        let array = store
            .sorted(by: { $0.key < $1.key })   // 确定性输出
            .map { word, usage -> [String: Any] in
                [
                    "word": word,
                    "count": usage.count,
                    "last_used_ts": usage.lastUsedTs,
                    "source": usage.source
                ]
            }
        guard let data = try? JSONSerialization.data(withJSONObject: array) else { return }
        try? data.write(to: url, options: .atomic)
    }
}
