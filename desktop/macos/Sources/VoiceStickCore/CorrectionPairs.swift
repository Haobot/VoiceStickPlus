import Foundation

// 纠错对资产化（用户数据飞轮 P0-1，自 Windows correction_pairs.{h,cc} 1:1 移植；对应
// Doc/Plan/user-data-flywheel-wetype-benchmark.md §2 与
// Doc/Plan/doubaoime-user-data-flywheel.md §3.1 改写对表）：每次精修产出
// original→refined 时提取最小替换片段对（如「鱼器渍→语气词」），落地为可累积的
// 本地规则资产；精修 prompt 注入 top-N few-shot，让同一错误第二次不犯。
// 存储先 JSON（schema 预留 SQLite 迁移：before/after/count/last_seen/source/app）；
// 多写者沿用 hotword_candidate_miner B13 模式：唯一写入口 + 进程级锁 + reload-merge-save。
//
// 只保留「替换」形态对（两侧均非空）：整句改写（超长侧）、删除/插入、纯标点
// 空白调整一律不入表——few-shot 需要明确的「错形→正形」锚点，删除/插入对留给
// 后续火山 correct_table 阶段（typeless 方案 P0-3-2）再评估。

/// 单侧最大码点数：超过即视为整句改写，不入表（防垃圾进垃圾出）。
public let kCorrectionPairMaxSideChars = 24
/// prompt few-shot 段上限对数（对齐 wetype 方案 §2「如 8 对」）。
public let kCorrectionPromptMaxPairs = 8
/// 存储容量硬上限（P1-4 完整清洗策略落地前的防膨胀闸）。
public let kCorrectionPairStoreCap = 1000

public struct CorrectionPair: Equatable {
    public var before: String             // ASR/精修前错误形式
    public var after: String              // 精修后用户认可形式
    public var count: Int                 // 累计出现次数（精修 diff 挖掘 seen 计数）
    public var useCount: Int              // 自动应用次数（doubao 方案 use_count，反馈环基底）
    public var recallCount: Int           // 应用后被用户保留次数（doubao recall_count，精确率 = recall/use）
    public var samePronunciation: Bool    // 同音/近音对（doubao P2 入表闸门：拼音校验前置）
    public var lastSeenTs: Int64          // 最近出现 epoch 秒
    public var source: String             // 产物来源（当前仅精修 diff）
    public var app: String                // 前台应用分桶（P1-2 接线，当前恒空=全局）

    public init(before: String, after: String, count: Int = 0, useCount: Int = 0,
                recallCount: Int = 0, samePronunciation: Bool = false, lastSeenTs: Int64 = 0,
                source: String = "refine", app: String = "") {
        self.before = before
        self.after = after
        self.count = count
        self.useCount = useCount
        self.recallCount = recallCount
        self.samePronunciation = samePronunciation
        self.lastSeenTs = lastSeenTs
        self.source = source
        self.app = app
    }

    /// doubao §3.3 降权判定：应用 ≥10 次且精确率（recall/use）< 0.5 → 自动应用池
    /// 摘除、few-shot 沉底（用户裁决前的保守隔离；设置页裁决随后续轮次）。
    public var isDemoted: Bool {
        guard useCount >= 10 else { return false }
        return Double(recallCount) / Double(max(useCount, 1)) < 0.5
    }
}

/// 一次提取产出的替换对（before→after）。独立于 CorrectionPair——后者带统计列，
/// 前者只是 diff 中间产物；结构体形态让数组可直接 Equatable 比较（Swift 元组不行）。
public struct CorrectionReplacement: Equatable {
    public let before: String
    public let after: String

    public init(before: String, after: String) {
        self.before = before
        self.after = after
    }
}

public enum CorrectionPairs {
    // ---- 码点工具（对齐 C++ 端 UTF-8 码点级语义）----

    private static func isAsciiSpace(_ scalar: Unicode.Scalar) -> Bool {
        return scalar == " " || scalar == "\t" || scalar == "\n" || scalar == "\r"
    }

    /// CJK 基本区/扩展A 或 ASCII 字母数字：过滤纯标点/空白调整（对 few-shot 无锚定价值）。
    private static func isContentChar(_ scalar: Unicode.Scalar) -> Bool {
        let value = scalar.value
        if (0x4E00...0x9FFF).contains(value) || (0x3400...0x4DBF).contains(value) { return true }
        guard scalar.isASCII else { return false }
        return CharacterSet.alphanumerics.contains(scalar)
    }

    private static func hasContentChar(_ cps: ArraySlice<Unicode.Scalar>) -> Bool {
        return cps.contains(where: isContentChar)
    }

    /// 去除首尾 ASCII 空白（diff 片段经常捎带周边空格，few-shot 锚点不需要）。
    private static func trimSpaces(_ cps: ArraySlice<Unicode.Scalar>) -> [Unicode.Scalar] {
        var begin = cps.startIndex
        var end = cps.endIndex
        while begin < end, isAsciiSpace(cps[begin]) { begin += 1 }
        while end > begin, isAsciiSpace(cps[end - 1]) { end -= 1 }
        return Array(cps[begin..<end])
    }

    private static func string(_ cps: [Unicode.Scalar]) -> String {
        return String(String.UnicodeScalarView(cps))
    }

    /// UTF-8 码点级最小编辑对齐：提取 refined 相对 original 被替换的连续片段对。
    /// 任一侧超过 512 码点（整句改写量级）返回空；同一对在一次提取内去重。
    public static func extractReplacementPairs(
        _ original: String, _ refined: String
    ) -> [CorrectionReplacement] {
        let maxCodepoints = 512
        let a = Array(original.unicodeScalars)
        let b = Array(refined.unicodeScalars)
        guard a.count <= maxCodepoints, b.count <= maxCodepoints else { return [] }
        // 任一侧为空 = 纯插入/删除（编辑追踪的纯追加编辑会产出空 before），无替换对；
        // 也挡住下方 1...n 循环在 n/m 为 0 时的无效区间崩溃。
        guard !a.isEmpty, !b.isEmpty else { return [] }

        // LCS（码点级）求最长公共子序列，未匹配的连续片段即替换对。
        let n = a.count
        let m = b.count
        let width = m + 1
        var dp = [UInt16](repeating: 0, count: (n + 1) * width)
        for i in 1...n {
            for j in 1...m {
                dp[i * width + j] = a[i - 1] == b[j - 1]
                    ? dp[(i - 1) * width + j - 1] &+ 1
                    : max(dp[(i - 1) * width + j], dp[i * width + j - 1])
            }
        }
        // 回溯收集匹配点（按 original 坐标升序还原）。
        var matches: [(Int, Int)] = []
        var i = n
        var j = m
        while i > 0, j > 0 {
            if a[i - 1] == b[j - 1] {
                matches.append((i - 1, j - 1))
                i -= 1
                j -= 1
            } else if dp[(i - 1) * width + j] >= dp[i * width + j - 1] {
                i -= 1
            } else {
                j -= 1
            }
        }
        matches.reverse()

        // 相邻匹配点之间的未匹配片段即一次替换；两侧均为空（纯匹配）跳过。
        var pairs: [CorrectionReplacement] = []
        var prevA = 0
        var prevB = 0
        func emit(endA: Int, endB: Int) {
            guard !(endA == prevA && endB == prevB) else { return }
            let before = trimSpaces(a[prevA..<endA])
            let after = trimSpaces(b[prevB..<endB])
            prevA = endA
            prevB = endB
            if before.isEmpty || after.isEmpty { return }                  // 删除/插入不入表
            if before.count > kCorrectionPairMaxSideChars ||
               after.count > kCorrectionPairMaxSideChars { return }        // 整句改写量级
            if !hasContentChar(before[...]) || !hasContentChar(after[...]) { return }  // 纯标点/空白
            pairs.append(CorrectionReplacement(before: string(before), after: string(after)))
        }
        for (ma, mb) in matches {
            emit(endA: ma, endB: mb)
            prevA = ma + 1
            prevB = mb + 1
        }
        emit(endA: n, endB: m)

        // 同一次提取内去重（保序）。
        var deduped: [CorrectionReplacement] = []
        for pair in pairs where !deduped.contains(pair) {
            deduped.append(pair)
        }
        return deduped
    }

    /// 内存合并：同 (before, after, app) 计数 +1 并刷新时间（对齐 doubao 方案
    /// UNIQUE(src,dst,app_bucket)——同对在不同 App 分桶各自累计）；新对追加；超容量
    /// 按（count 升序、last_seen 升序、字典序）逐出。
    public static func record(
        _ store: inout [CorrectionPair], pairs: [CorrectionReplacement], nowTs: Int64,
        source: String = "refine", app: String = ""
    ) {
        for replacement in pairs {
            if let index = store.firstIndex(where: {
                $0.before == replacement.before && $0.after == replacement.after && $0.app == app
            }) {
                store[index].count += 1
                store[index].lastSeenTs = nowTs
                continue
            }
            store.append(CorrectionPair(
                before: replacement.before, after: replacement.after,
                count: 1,
                samePronunciation: PinyinGuard.sameOrNearText(replacement.before, replacement.after),
                lastSeenTs: nowTs, source: source, app: app
            ))
        }
        // 容量硬上限：count 升序、last_seen 升序、字典序稳定逐出。
        if store.count > kCorrectionPairStoreCap {
            let order = store.enumerated().sorted { lhs, rhs in
                if lhs.element.count != rhs.element.count { return lhs.element.count < rhs.element.count }
                if lhs.element.lastSeenTs != rhs.element.lastSeenTs {
                    return lhs.element.lastSeenTs < rhs.element.lastSeenTs
                }
                if lhs.element.before != rhs.element.before { return lhs.element.before < rhs.element.before }
                return lhs.element.after < rhs.element.after
            }
            let evict = Set(order.prefix(store.count - kCorrectionPairStoreCap).map(\.offset))
            store = store.enumerated().filter { !evict.contains($0.offset) }.map(\.element)
        }
    }

    /// JSON 读取（{"version":1,"pairs":[...]}；缺失/损坏返回空 store）。
    public static func load(url: URL) -> [CorrectionPair] {
        guard let data = try? Data(contentsOf: url),
              let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let array = root["pairs"] as? [[String: Any]] else {
            return []
        }
        var store: [CorrectionPair] = []
        for item in array {
            guard let before = item["before"] as? String, !before.isEmpty,
                  let after = item["after"] as? String, !after.isEmpty else { continue }
            var pair = CorrectionPair(before: before, after: after)
            if let count = item["count"] as? Int { pair.count = count }
            if let useCount = item["use_count"] as? Int { pair.useCount = useCount }
            if let recallCount = item["recall_count"] as? Int { pair.recallCount = recallCount }
            if let same = item["same_pronunciation"] as? Bool { pair.samePronunciation = same }
            if let ts = item["last_seen_ts"] as? Int64 { pair.lastSeenTs = ts }
            if let ts = item["last_seen_ts"] as? Int { pair.lastSeenTs = Int64(ts) }
            if let source = item["source"] as? String, !source.isEmpty { pair.source = source }
            if let app = item["app"] as? String { pair.app = app }
            store.append(pair)
        }
        return store
    }

    /// JSON 写入（保存前按 (before, after, app) 字典序排序，输出确定性；best-effort，写失败静默）。
    public static func save(_ store: [CorrectionPair], url: URL) {
        let sorted = store.sorted { lhs, rhs in
            if lhs.before != rhs.before { return lhs.before < rhs.before }
            if lhs.after != rhs.after { return lhs.after < rhs.after }
            return lhs.app < rhs.app
        }
        let array: [[String: Any]] = sorted.map { pair in
            [
                "before": pair.before,
                "after": pair.after,
                "count": pair.count,
                "use_count": pair.useCount,
                "recall_count": pair.recallCount,
                "same_pronunciation": pair.samePronunciation,
                "last_seen_ts": pair.lastSeenTs,
                "source": pair.source,
                "app": pair.app
            ]
        }
        guard let data = try? JSONSerialization.data(withJSONObject: ["version": 1, "pairs": array]) else { return }
        try? data.write(to: url, options: .atomic)
    }

    // B13 单写入口：进程级互斥 + 每次从磁盘重读后合并落盘（绝不使用陈旧快照写回）。
    private static let fileLock = NSLock()

    public static func recordToDisk(
        url: URL, pairs: [CorrectionReplacement], nowTs: Int64, source: String = "refine",
        app: String = ""
    ) {
        guard !pairs.isEmpty else { return }
        fileLock.lock()
        defer { fileLock.unlock() }
        var store = load(url: url)
        record(&store, pairs: pairs, nowTs: nowTs, source: source, app: app)
        save(store, url: url)
    }

    /// 应用位置 1（doubao 方案 §3.1）：全句精确命中——文本（去首尾空白）与某改写对
    /// before 完全相等时返回该对（跳过 LLM 直接替换的确定性路径）。分桶查询口径
    /// （§3.4）：app 非空时候选 = 当前应用桶 ∪ 全局桶（其他 App 的桶不参与），当前
    /// 桶优先；低精确率降权对（§3.3，isDemoted）从自动应用池摘除；桶内取（count
    /// 降序、last_seen 降序、字典序）最高者；count<=0 剔除。
    public static func exactReplacement(
        for text: String, in store: [CorrectionPair], app: String? = nil
    ) -> CorrectionPair? {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return nil }
        return store
            .filter { entry in
                guard entry.count > 0, !entry.isDemoted,
                      entry.before.trimmingCharacters(in: .whitespacesAndNewlines) == trimmed else {
                    return false
                }
                return isBucketMatch(entry.app, app: app)
            }
            .sorted { lhs, rhs in
                if isCurrentBucket(lhs.app, app: app) != isCurrentBucket(rhs.app, app: app) {
                    return isCurrentBucket(lhs.app, app: app)
                }
                if lhs.count != rhs.count { return lhs.count > rhs.count }
                if lhs.lastSeenTs != rhs.lastSeenTs { return lhs.lastSeenTs > rhs.lastSeenTs }
                if lhs.before != rhs.before { return lhs.before < rhs.before }
                return lhs.after < rhs.after
            }
            .first
    }

    /// 分桶匹配：app 为空/nil（未分工桶）= 全局查询；否则当前应用桶 ∪ 全局桶。
    private static func isBucketMatch(_ entryApp: String, app: String?) -> Bool {
        guard let app, !app.isEmpty else { return entryApp.isEmpty }
        return entryApp == app || entryApp.isEmpty
    }

    private static func isCurrentBucket(_ entryApp: String, app: String?) -> Bool {
        guard let app, !app.isEmpty else { return false }
        return entryApp == app
    }

    /// B13 单写入口：精确命中应用后 use_count+1 落盘（doubao 方案 use_count，
    /// P3 反馈环的召回侧基底）。
    public static func markUsedOnDisk(url: URL, pair: CorrectionPair, nowTs: Int64) {
        fileLock.lock()
        defer { fileLock.unlock() }
        var store = load(url: url)
        guard let index = store.firstIndex(where: {
            $0.before == pair.before && $0.after == pair.after && $0.app == pair.app
        }) else { return }
        store[index].useCount += 1
        store[index].lastSeenTs = nowTs
        save(store, url: url)
    }

    /// B13 单写入口：应用后用户确认保留（粘贴走完未取消）→ recall_count+1
    /// （doubao §3.3 保留信号；取消/回退不增——调用方只在确认路径调）。
    public static func markRecalledOnDisk(url: URL, pair: CorrectionPair) {
        fileLock.lock()
        defer { fileLock.unlock() }
        var store = load(url: url)
        guard let index = store.firstIndex(where: {
            $0.before == pair.before && $0.after == pair.after && $0.app == pair.app
        }) else { return }
        store[index].recallCount += 1
        save(store, url: url)
    }

    /// prompt few-shot 候选：分桶查询（doubao §3.4）——app 非空时候选 = 当前应用桶
    /// ∪ 全局桶，当前桶优先占预算；桶内按 count 降序、last_seen 降序、字典序；
    /// 取前 maxPairs；count<=0 剔除。
    public static func trimForPrompt(
        _ store: [CorrectionPair], maxPairs: Int, app: String? = nil
    ) -> [CorrectionPair] {
        guard maxPairs > 0 else { return [] }
        return store
            .filter { $0.count > 0 && isBucketMatch($0.app, app: app) }
            .sorted { lhs, rhs in
                if isCurrentBucket(lhs.app, app: app) != isCurrentBucket(rhs.app, app: app) {
                    return isCurrentBucket(lhs.app, app: app)
                }
                // 同音/近音对优先（doubao §3.2：同音纠错最安全、复用价值最高）。
                if lhs.samePronunciation != rhs.samePronunciation {
                    return lhs.samePronunciation
                }
                // 低精确率降权对沉底（§3.3：从自动应用池摘除后的 few-shot 保守位）。
                if lhs.isDemoted != rhs.isDemoted { return !lhs.isDemoted }
                if lhs.count != rhs.count { return lhs.count > rhs.count }
                if lhs.lastSeenTs != rhs.lastSeenTs { return lhs.lastSeenTs > rhs.lastSeenTs }
                if lhs.before != rhs.before { return lhs.before < rhs.before }
                return lhs.after < rhs.after
            }
            .prefix(maxPairs)
            .map { $0 }
    }

    /// few-shot 段构造（两端逐字一致，双端单测钉死）。空集或全部被格式安全过滤
    /// （含引号/换行的对）时返回空串。
    public static func promptSection(_ pairs: [CorrectionPair]) -> String {
        if pairs.isEmpty { return "" }
        var section = "\n\n用户历史纠正（输出时请遵循这些偏好：仅当识别文本出现与纠正前写法相同或同音、"
            + "近音的错误时替换为纠正后写法，其余内容不得改动）："
        var appended = 0
        for pair in pairs {
            // 格式安全过滤：含引号/换行的对会破坏行式 few-shot 结构，跳过。
            if pair.before.isEmpty || pair.after.isEmpty { continue }
            if pair.before.contains("\"") || pair.after.contains("\"") { continue }
            if pair.before.contains("\n") || pair.after.contains("\n") { continue }
            section += "\n- \"\(pair.before)\" → \"\(pair.after)\""
            appended += 1
        }
        if appended == 0 { return "" }
        return section
    }
}
