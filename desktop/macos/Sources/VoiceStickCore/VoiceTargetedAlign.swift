import Foundation

// targeted 目标词本地对齐（候选约束式）——ASR 把编辑指令里的目标词听漂（同音/近音/
// 掉字）时的兜底层。设计对齐 Windows 跨轮纠错的「模型只出指令、守卫代码层校验执行」
// 架构（Doc/Plan/local-asr-accuracy-and-cross-turn-refinement.md）：
//   1. 拼音预筛（纯逻辑、PinyinGuard 同表口径的分级评分）从最近注入 remainingText
//      生成 top-K 候选**子串**；
//   2. 本地小模型（ollama /api/chat + JSON-schema enum 硬约束）从候选中挑一个或 NONE；
//   3. 输出必须精确命中候选枚举，任何失败回退现行「普通文本粘贴」路径。
// 安全不变式：候选永远是 remainingText 的真实子串 ⇒ 对齐后的编辑仍走原 verified
// 选区/读回/终验链，模型无法制造文本里不存在的编辑对象；默认关闭
// （[voice_edit].local_align_enabled），见 Doc/Plan/voice-edit-commands.md targeted 章。
//
// 本文件纯逻辑（Foundation only）：候选生成/评分/prompt 构造/输出解析；HTTP 调用在
// App 层（LocalAlignClient），协调器经闭包注入。Windows 侧同构落点
// voice_targeted_align.{h,cc}（复用 llama.cpp 常驻引擎 + pinyin_guard，见方案）。

/// 候选数与窗口长度的双端一致常量。
public let kVoiceTargetedAlignMaxCandidates = 8
/// 窗口长度 = 目标 scalar 数 ±1（真实案例：成都→都 掉首字、背景→北京 等长同音）。
public let kVoiceTargetedAlignWindowDelta = 1
/// 默认入选分数（分级拼音分，见 charScore；预筛偏召回，精度交给模型+枚举约束）。
/// 实测定标：0.4 让「分析/升旗」(0.45) 入选，同时「长城/上班」(0.275) 这类完全
/// 无关等长对仍被挡在门外；负例残余的单词乱配交给模型判 NONE。
public let kVoiceTargetedAlignDefaultMinScore = 0.4

public enum VoiceTargetedAlign {
    /// 一个对齐候选：remainingText 的连续片段 + 拼音相似分 + UTF-16 起点（诊断用）。
    public struct Candidate: Equatable {
        public let text: String
        public let score: Double
        public let utf16Start: Int

        public init(text: String, score: Double, utf16Start: Int) {
            self.text = text
            self.score = score
            self.utf16Start = utf16Start
        }
    }

    // MARK: - 评分（PinyinGuard 同表、分级化）

    /// 单字分级相似度（PinyinData 多读音并集口径，无调）：
    ///   码点相同 1.0；声韵全同 0.9（同音）；韵母命中模糊对且声同 0.8（近音 an/ang）；
    ///   仅韵母同 0.55（M0 spike「声母不参与」口径，降权保留）；
    ///   仅韵母模糊对（声母不同，分/升 en-eng）0.35（候选召回档，弱信号不弃）；
    ///   其余 0。双方均非汉字时按 ASCII 折叠相等（1.0/0）；仅一方汉字 0；表外汉字 0。
    public static func charScore(_ a: Unicode.Scalar, _ b: Unicode.Scalar) -> Double {
        if a == b { return 1.0 }
        let isHan: (Unicode.Scalar) -> Bool = { $0.value >= 0x4e00 && $0.value <= 0x9fff }
        guard isHan(a), isHan(b),
              let ea = PinyinData.find(a.value), let eb = PinyinData.find(b.value) else {
            if !isHan(a) && !isHan(b) {
                let lower: (Unicode.Scalar) -> Unicode.Scalar = {
                    ($0.value >= 65 && $0.value <= 90) ? Unicode.Scalar($0.value + 32)! : $0
                }
                return lower(a) == lower(b) ? 1.0 : 0.0
            }
            return 0.0
        }
        let finalsIntersect = setsIntersect(ea.finals, eb.finals)
        let initialsIntersect = setsIntersect(ea.initials, eb.initials)
        if initialsIntersect && finalsIntersect { return 0.9 }
        if initialsIntersect && fuzzyFinalHit(ea, eb) { return 0.8 }
        if finalsIntersect { return 0.55 }
        if fuzzyFinalHit(ea, eb) { return 0.35 }
        return 0.0
    }

    /// 文本对齐分：等长逐位均值；长度恰差 1 取删长方一字的最优均值；其余 0。
    /// 真实口径：「背景/北京」等长同音 ≈0.9；「分析/升旗」= 析/旗 韵母同 0.55、
    /// 分/升 0 ⇒ 0.275——预筛按相对排名保召回，绝对分只挡完全无关节段。
    public static func alignmentScore(_ target: String, _ window: String) -> Double {
        let a = Array(target.unicodeScalars)
        let b = Array(window.unicodeScalars)
        let delta = a.count - b.count
        if delta == 0 {
            guard !a.isEmpty else { return 0.0 }
            var total = 0.0
            for (x, y) in zip(a, b) { total += charScore(x, y) }
            return total / Double(a.count)
        }
        guard abs(delta) == kVoiceTargetedAlignWindowDelta else { return 0.0 }
        // 长方删每个位置后与短方等长对齐，取最优。
        let (long, short) = delta < 0 ? (b, a) : (a, b)
        var best = 0.0
        for drop in long.indices {
            var trimmed = long
            trimmed.remove(at: drop)
            var total = 0.0
            for (x, y) in zip(trimmed, short) { total += charScore(x, y) }
            best = max(best, total / Double(short.count))
        }
        return best
    }

    // MARK: - 候选生成

    /// 从 remainingText 生成对齐候选：scalar 连续窗口，长度 ∈ [targetLen-1, targetLen+1]，
    /// alignmentScore ≥ minScore；**重叠簇内只保留与目标等长的候选**（无等长候选的簇
    /// 保留最佳 1 个）——否则「升旗/看升旗/旗」「北京/去北京/北京上」这类同位重叠变体
    /// 会让模型在词边界上犯错（实测全模型如此）。簇间按最高分降序取前 maxCandidates 个。
    /// exact 目标本身命中时它必为 1.0 分首位（调用方门控已排除 exact 出现，防御性保留）。
    public static func candidates(
        asrTarget: String, remainingText: String,
        minScore: Double = Double(kVoiceTargetedAlignDefaultMinScore),
        maxCandidates: Int = kVoiceTargetedAlignMaxCandidates
    ) -> [Candidate] {
        let targetScalars = Array(asrTarget.unicodeScalars)
        let textScalars = Array(remainingText.unicodeScalars)
        guard !targetScalars.isEmpty, !textScalars.isEmpty else { return [] }
        // UTF-16 偏移表：offsets[i] = 前 i 个 scalar 的 UTF-16 长度（与 Matcher 同思路）。
        var offsets = [Int](repeating: 0, count: textScalars.count + 1)
        for index in textScalars.indices {
            offsets[index + 1] = offsets[index]
                + String(String.UnicodeScalarView([textScalars[index]])).utf16.count
        }
        let minLength = max(1, targetScalars.count - kVoiceTargetedAlignWindowDelta)
        let maxLength = min(
            targetScalars.count + kVoiceTargetedAlignWindowDelta,
            kVoiceTargetedMaxTargetGraphemes
        )
        var windows: [Candidate] = []
        for length in minLength...maxLength where length <= textScalars.count {
            var start = 0
            while start + length <= textScalars.count {
                let window = Array(textScalars[start..<start + length])
                // 窗口字符集闸：CJK 基本区汉字与 ASCII 字母数字；空格仅当窗口
                // 含字母数字时放行（英文短语「VS Code」是合法目标，中文文本里
                // 的孤立空格窗口仍被挡）。
                var hasAlnum = false
                var windowIsWord = true
                for scalar in window {
                    let v = scalar.value
                    if (v >= 48 && v <= 57) || (v >= 65 && v <= 90) || (v >= 97 && v <= 122) {
                        hasAlnum = true
                    } else if !(v >= 0x4e00 && v <= 0x9fff) && v != 32 {
                        windowIsWord = false
                        break
                    }
                }
                if windowIsWord && (!window.contains(where: { $0.value == 32 }) || hasAlnum) {
                    let text = String(String.UnicodeScalarView(window))
                    let score = alignmentScore(asrTarget, text)
                    // 非等长窗口（len±1）阈值提到 0.9：实测负例垃圾候选几乎全是
                    // 非等长单词（上/今/美/里 0.5~0.55 分），而真实漂移案例（背景→
                    // 北京、分析→升旗）都是等长窗口——分层阈值把负例消灭在预筛层。
                    let effectiveMin = length == targetScalars.count ? minScore : 0.9
                    if score >= effectiveMin {
                        windows.append(Candidate(text: text, score: score, utf16Start: offsets[start]))
                    }
                }
                start += 1
            }
        }
        // 重叠簇收敛：按 UTF-16 区间重叠分簇；簇内有 |len| == targetLen 的候选时只留
        // 等长者（分数最高、位置最先），否则留最佳 1 个。簇间按簇内最高分降序。
        let sortedWindows = windows.sorted {
            if $0.score != $1.score { return $0.score > $1.score }
            return $0.utf16Start < $1.utf16Start
        }
        struct Cluster { var pick: Candidate; var bestScore: Double; var bestLenMatch: Bool }
        var clusters: [Cluster] = []
        for candidate in sortedWindows {
            let length = candidate.text.unicodeScalars.count
            let utf16End = candidate.utf16Start + candidate.text.utf16.count
            let lenMatches = length == targetScalars.count
            if let index = clusters.firstIndex(where: { other in
                let otherEnd = other.pick.utf16Start + other.pick.text.utf16.count
                return candidate.utf16Start < otherEnd && other.pick.utf16Start < utf16End
            }) {
                // 同簇：等长者可替换非等长代表；等长内部保持分高者。
                let curLen = clusters[index].pick.text.unicodeScalars.count
                let curLenMatches = curLen == targetScalars.count
                if lenMatches && !curLenMatches {
                    clusters[index].pick = candidate
                } else if lenMatches == curLenMatches && candidate.score > clusters[index].pick.score {
                    clusters[index].pick = candidate
                }
                clusters[index].bestScore = max(clusters[index].bestScore, candidate.score)
                clusters[index].bestLenMatch = clusters[index].bestLenMatch || lenMatches
            } else {
                clusters.append(Cluster(pick: candidate, bestScore: candidate.score, bestLenMatch: lenMatches))
            }
        }
        return clusters
            .sorted {
                if $0.bestScore != $1.bestScore { return $0.bestScore > $1.bestScore }
                return $0.pick.utf16Start < $1.pick.utf16Start
            }
            .prefix(maxCandidates)
            .map { $0.pick }
    }

    // MARK: - prompt / 请求构造（双端同构；App 层负责 HTTP）

    /// few-shot 示例（M0 spike 教训：0.6B/1.7B 级对指令式 prompt 无法跟随，few-shot
    /// 是硬要求；assistant 侧带真实短理由——占位符「…」会教坏模型（实测 qwen3 全体
    /// NONE 崩塌）。正例=同音漂移推理，负例=无关词与同族词两种 NONE 形态。
    private static let fewShot: [(user: String, reason: String, answer: String)] = [
        ("上屏：我今天去北京上班。\n指令：把背景改成伤害。\n目标词：背景\n候选：[北京, 上班]",
         "背景与北京同音，用户想改的是北京", "北京"),
        ("上屏：我要去北京看升旗。\n指令：把分析改成吃烤鸭。\n目标词：分析\n候选：[升旗, 北京]",
         "分析接近升旗的发音，上文刚说过升旗", "升旗"),
        ("上屏：好的，我现在又来到了成都，这里有很多美食。\n指令：把每时改成佳肴。\n目标词：每时\n候选：[美食, 成都]",
         "每时与美食同音，改成佳肴指的应是美食", "美食"),
        ("上屏：我今天去北京上班。\n指令：把长城改成故宫。\n目标词：长城\n候选：[北京, 上班]",
         "长城与候选里的词发音都不同，与指令无关", "NONE"),
        ("上屏：会议安排在星期一和星期三。\n指令：把星期八改成星期天。\n目标词：星期八\n候选：[星期一, 星期三]",
         "星期八与星期一只是同族词，不是听错的样子", "NONE")
    ]

    public static let systemPrompt = """
    你是语音编辑指令的目标词对齐器。用户上一句已输入的文本见「上屏」，接着说了一条编辑指令；\
    语音识别可能把指令里的目标词听成同音或近音的别的词。任务：判断「目标词」是不是「上屏」文本\
    中某个词被听错的样子——是则从「候选」里选出那个词；不是、或候选只是碰巧发音相近但与指令\
    无关时，answer 给 NONE。宁可 NONE 不可乱选。先在 reason 里用一句话说明判断依据，再把结论\
    写进 answer；answer 只能是候选词原样或 NONE。
    """

    /// 查询 prompt 主体（不含「输出：」尾缀也行，few-shot 已建立形态；保留尾缀）。
    public static func userPrompt(
        command: String, asrTarget: String, remainingText: String, candidates: [String]
    ) -> String {
        "上屏：\(remainingText)\n指令：\(command)\n目标词：\(asrTarget)\n候选：[\(candidates.joined(separator: ", "))]\n输出："
    }

    /// ollama /api/chat 请求体（字典形态；JSON 由调用方序列化）。
    /// format 为结构化 schema：reason（一句话判断依据）+ answer（候选+NONE 枚举硬
    /// 约束）。实测纯枚举直出会让小模型把任务退化成「挑最像的候选」而不敢 NONE；
    /// 给一个 reason 槽先写依据再下结论，NONE 判断显著改善（见评测报告）。
    public static func chatRequestBody(
        model: String, command: String, asrTarget: String, remainingText: String,
        candidates: [String], temperature: Double = 0, maxTokens: Int = 64
    ) -> [String: Any] {
        var messages: [[String: Any]] = [["role": "system", "content": systemPrompt]]
        for shot in fewShot {
            messages.append(["role": "user", "content": shot.user])
            let reasonJSON = shot.reason
                .replacingOccurrences(of: "\\", with: "\\\\")
                .replacingOccurrences(of: "\"", with: "\\\"")
            messages.append([
                "role": "assistant",
                "content": "{\"reason\":\"\(reasonJSON)\",\"answer\":\"\(shot.answer)\"}"
            ])
        }
        messages.append([
            "role": "user",
            "content": userPrompt(
                command: command, asrTarget: asrTarget,
                remainingText: remainingText, candidates: candidates)
        ])
        return [
            "model": model,
            "messages": messages,
            "stream": false,
            "think": false,
            "format": [
                "type": "object",
                "properties": [
                    "reason": ["type": "string"],
                    "answer": ["type": "string", "enum": candidates + ["NONE"]],
                ],
                "required": ["answer"],
            ],
            "options": ["temperature": temperature, "num_predict": maxTokens]
        ] as [String: Any]
    }

    // MARK: - 输出解析

    /// 解析模型输出：优先按结构化 JSON 取 answer；回退纯文本路径（剥空白与成对
    /// 引号后精确等于某候选 → 该候选；等于 NONE → "NONE"；其余 nil）。
    /// ollama enum 约束下 content 可能带 JSON 引号，两种形态都兼容。
    public static func parseAnswer(_ raw: String, candidates: [String]) -> String? {
        let trimmed = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        if let data = trimmed.data(using: .utf8),
           let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
           let answer = object["answer"] as? String {
            if answer == "NONE" || answer == "none" { return "NONE" }
            if candidates.contains(answer) { return answer }
            return nil
        }
        var text = trimmed
        for pair in [("\"", "\""), ("「", "」"), ("‘", "’"), ("“", "”")] {
            if text.hasPrefix(pair.0), text.hasSuffix(pair.1), text.count >= 2 {
                text = String(text.dropFirst().dropLast())
                    .trimmingCharacters(in: .whitespacesAndNewlines)
            }
        }
        if text == "NONE" || text == "none" { return "NONE" }
        return candidates.first { $0 == text }
    }

    // MARK: - 拼音工具（PinyinGuard 同口径的小型复刻；其自身接口是布尔判定，
    // 分级评分需要直接访问声/韵母集合，且模糊对表要与 Windows pinyin_guard 常量同源）

    private static let fuzzyFinalPairs: [(String, String)] = [
        ("an", "ang"), ("en", "eng"), ("in", "ing"),
        ("ian", "iang"), ("uan", "uang"), ("e", "i")
    ]

    private static func listContains(_ list: String, _ item: String) -> Bool {
        list.split(separator: ",").contains { String($0) == item }
    }

    private static func setsIntersect(_ a: String, _ b: String) -> Bool {
        a.split(separator: ",").contains { listContains(b, String($0)) }
    }

    private static func fuzzyFinalHit(_ ea: PinyinEntry, _ eb: PinyinEntry) -> Bool {
        for (x, y) in fuzzyFinalPairs {
            if (listContains(ea.finals, x) && listContains(eb.finals, y))
                || (listContains(ea.finals, y) && listContains(eb.finals, x)) {
                return true
            }
        }
        return false
    }
}
