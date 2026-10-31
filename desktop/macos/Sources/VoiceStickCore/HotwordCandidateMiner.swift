import Foundation

// 热词候选挖掘（用户数据飞轮 P0-2，自 Windows hotword_candidate_miner.{h,cc} 1:1
// 移植，补齐 macOS 缺失的挖掘管道；存储格式与 Windows hotword_candidates.json 一致）。
//
// 纠错对挖掘（热词候选）：精修把 ASR 错词纠回正确形式时，正确形式若是「标识符
// 样式」且不在热词表里，就是候选新热词。累计出现 kHotwordCandidateThreshold 次后
// 向用户提示，由用户在设置页确认后才入表。
//
// 为什么不做全自动入表（Windows 同源哲学）：LLM 可能把错误变体（如 clouddmd）当
// 热词加进去，之后 ASR 第一遍反而被错误热词带偏，形成负反馈；且热词表会无限膨胀，
// 稀释直传（双向流式 ~100 tokens）与精修 prompt 的效果。挖掘只产候选，入表必须经
// 用户确认。

/// 达到该出现次数后向用户建议。
public let kHotwordCandidateThreshold = 3

public struct HotwordCandidateStore: Equatable {
    /// 候选词 -> 累计出现次数
    public var counts: [String: Int] = [:]
    /// 用户在设置里忽略的词，永不建议
    public var dismissed: Set<String> = []
    /// 已提示过的词，避免重复打扰（设置里仍可见）
    public var notified: Set<String> = []

    public init() {}
}

public enum HotwordCandidateMiner {
    /// 标识符字符集：ASCII 字母数字 + ._-（对齐 Windows IsTokenChar 的 ASCII 口径，
    /// CJK 等非 ASCII 字符一律视为分隔符）。
    private static func isTokenChar(_ scalar: Unicode.Scalar) -> Bool {
        guard scalar.isASCII else { return false }
        return CharacterSet.alphanumerics.contains(scalar)
            || scalar == "." || scalar == "_" || scalar == "-"
    }

    /// 标识符样式判定：字母开头（排除 "2.0" 这类版本号/纯数字），长度 >= 3，且至少
    /// 含一个大写字母/数字/./_/-（排除普通小写英文单词）。
    private static func looksLikeIdentifier(_ token: String) -> Bool {
        guard token.count >= 3 else { return false }
        let scalars = Array(token.unicodeScalars)
        guard let first = scalars.first,
              CharacterSet.letters.contains(first), first.isASCII else { return false }
        for scalar in scalars {
            if scalar == "." || scalar == "_" || scalar == "-" { return true }
            guard scalar.isASCII else { continue }
            if CharacterSet.uppercaseLetters.contains(scalar) { return true }
            if CharacterSet.decimalDigits.contains(scalar) { return true }
        }
        return false
    }

    /// 提取文本中标识符样式的拉丁词；尾部的句号/下划线/连字符会被剥掉
    /// （如句末 "hello." 的 "."）。
    public static func extractIdentifierTokens(_ text: String) -> [String] {
        var tokens: [String] = []
        var current = String.UnicodeScalarView()
        func flush() {
            while let last = current.last, last == "." || last == "_" || last == "-" {
                current.removeLast()
            }
            let token = String(current)
            if looksLikeIdentifier(token) { tokens.append(token) }
            current = String.UnicodeScalarView()
        }
        for scalar in text.unicodeScalars {
            if isTokenChar(scalar) {
                current.append(scalar)
            } else {
                flush()
            }
        }
        flush()
        return tokens
    }

    /// 挖掘候选：出现在 refined 但不在 original、不在 existing_hotwords 中的标识符，去重。
    public static func mineRefinementCandidates(
        original: String, refined: String, existingHotwords: [String]
    ) -> [String] {
        var candidates: [String] = []
        var seen: Set<String> = []
        for token in extractIdentifierTokens(refined) {
            guard seen.insert(token).inserted else { continue }
            if original.contains(token) { continue }
            if existingHotwords.contains(token) { continue }
            candidates.append(token)
        }
        return candidates
    }

    /// JSON 读取（{"counts":{...},"dismissed":[...],"notified":[...]}，与 Windows
    /// hotword_candidates.json 同构；缺失/损坏返回空 store）。
    public static func load(url: URL) -> HotwordCandidateStore {
        guard let data = try? Data(contentsOf: url),
              let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            return HotwordCandidateStore()
        }
        var store = HotwordCandidateStore()
        if let counts = root["counts"] as? [String: Any] {
            for (word, value) in counts {
                if let count = value as? Int { store.counts[word] = count }
            }
        }
        for key in ["dismissed", "notified"] {
            guard let array = root[key] as? [String] else { continue }
            if key == "dismissed" {
                store.dismissed = Set(array)
            } else {
                store.notified = Set(array)
            }
        }
        return store
    }

    /// JSON 写入（键序确定性输出；best-effort，写失败静默）。
    public static func save(_ store: HotwordCandidateStore, url: URL) {
        let object: [String: Any] = [
            "counts": store.counts,
            "dismissed": store.dismissed.sorted(),
            "notified": store.notified.sorted()
        ]
        guard let data = try? JSONSerialization.data(withJSONObject: object) else { return }
        try? data.write(to: url, options: .atomic)
    }

    /// 记录一批候选词，返回本次新晋达到阈值、且未忽略/未通知的词（调用方据此提示，
    /// 提示后 recordToDisk 自行把词加入 notified）。
    public static func record(
        _ store: inout HotwordCandidateStore, words: [String]
    ) -> [String] {
        var newlySuggested: [String] = []
        for word in words {
            if store.dismissed.contains(word) || store.notified.contains(word) { continue }
            let before = store.counts[word] ?? 0
            let after = before + 1
            store.counts[word] = after
            if before < kHotwordCandidateThreshold && after >= kHotwordCandidateThreshold {
                newlySuggested.append(word)
            }
        }
        return newlySuggested
    }

    /// 当前待确认建议（达到阈值且未忽略），按字典序（对齐 Windows std::map 遍历序）。
    public static func pendingSuggestions(_ store: HotwordCandidateStore) -> [String] {
        return store.counts
            .filter { $0.value >= kHotwordCandidateThreshold && !store.dismissed.contains($0.key) }
            .keys
            .sorted()
    }

    // ---- B13：候选文件的**唯一写入口**（从第一行起就是单写者，不走 Windows 的
    // load-once 缓存弯路）——「进程级互斥 + 每次从磁盘重读后改写」，全部文件变更
    // 收敛到本模块；后台挖掘与设置页同走这三个事务。 ----

    private static let fileLock = NSLock()

    /// 挖掘：记录候选并返回本次新晋建议（已达阈值、未忽略、未通知；返回前已记入
    /// notified，避免重复提示）。
    public static func recordToDisk(url: URL, words: [String]) -> [String] {
        fileLock.lock()
        defer { fileLock.unlock() }
        var store = load(url: url)
        let suggestions = record(&store, words: words)
        for word in suggestions { store.notified.insert(word) }
        save(store, url: url)
        return suggestions
    }

    /// 设置页「加入」：从 counts/notified 移除（词已进入热词表）。
    public static func consumeOnDisk(url: URL, word: String) -> Bool {
        fileLock.lock()
        defer { fileLock.unlock() }
        var store = load(url: url)
        let known = store.counts[word] != nil || store.notified.contains(word)
            || store.dismissed.contains(word)
        store.counts.removeValue(forKey: word)
        store.notified.remove(word)
        save(store, url: url)
        return known
    }

    /// 设置页「忽略」：记入 dismissed（永不建议）并移除 counts/notified。
    public static func dismissOnDisk(url: URL, word: String) -> Bool {
        fileLock.lock()
        defer { fileLock.unlock() }
        var store = load(url: url)
        store.dismissed.insert(word)
        store.counts.removeValue(forKey: word)
        store.notified.remove(word)
        save(store, url: url)
        return true
    }
}
