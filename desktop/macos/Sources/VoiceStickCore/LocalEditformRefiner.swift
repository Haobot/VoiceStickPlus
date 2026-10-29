import Foundation

/// 本地 editform 文本精修（macOS 本地 ASR 会话专用；M2 剩余部分，2026-10-29）。
///
/// 三层管线（评测最优组合 `det v5 + editform×Qwen2.5-3B` = CER 1.67%、damage 0，
/// 见 Doc/Test/local-fullstack-eval-2026-10-28.md；提示词/守卫从评测 harness
/// `run_refine_bench.py` **逐字节移植**——task-shape 纪律：改形态必须重评测）：
///   L1 确定性清理（`deterministicClean`：句首填充词、3+ 连字口吃、CJK 间空格）；
///   L2 editform：模型只输出「错词→热词原形 / 删除：片段」指令行；
///   L3 守卫执行（`applyEditInstructions`：替换目标必须命中热词表、错词/片段
///      必须真实存在于原文——「模型出指令、守卫代码执行」房规，damage=0 的保证）。
/// 引擎缺失/失败/超预算：逐层回退，最差不劣于 L1（Windows LocalRefinementClient
/// 同语义）；云端会话不走本类（makeRefiner 按会话路由）。
public final class LocalEditformRefiner: RefinerServing {

    // MARK: - 提示词（评测 EDITFORM_PROMPT 逐字节移植，勿改——改动即换形态）

    /// editform system prompt 骨架（few-shot 内嵌于 system；评测口径）。
    /// 热词表动态拼接在末尾（评测 VARIANTS["editform"] 同式：`"\n\n热词表：" + 、连接`）。
    public static func editformSystemPrompt(hotwords: [String]) -> String {
        """
        你是语音识别结果纠错器。输入识别文本与用户热词表，只输出修改指令，每行一条，别的不写：\
        1) 替换：错词→热词原形。错词是文本中实际出现的错误写法（同音/近音/大小写/分写/连写变形），\
        箭头右边必须是热词表里的词。\
        2) 删除：原样摘录要删除的无意义片段（填充词、口吃重复，如 嗯、啊、那个、我我）。\
        没有要改的只输出：无。禁止重写整句，禁止输出指令之外的任何内容。

        热词表：AGENTS.md、GitHub、蓝牙
        输入：嗯，帮我编辑 agentsdmd 这个文件，然后用蓝崖遥控器测试。
        处理：
        删除：嗯，
        agentsdmd→AGENTS.md
        蓝崖→蓝牙

        热词表：SDK、IDE
        输入：把skd装进die里面。
        处理：
        skd→SDK
        die→IDE

        热词表：Ogg、ASR
        输入：把ASR的音频封装成OG格式再发送。
        处理：
        OG→Ogg
        """ + "\n\n热词表：" + hotwords.joined(separator: "、")
    }

    /// 守卫最大指令数（评测 _MAX_EDITS=5）。
    static let maxEdits = 5

    // MARK: - L3 守卫（评测 apply_edit_instructions 移植）

    /// 守卫执行 editform 指令：替换目标必须命中热词表（casefold 归一），错词/
    /// 片段必须真实存在于文本。任何不合法指令行静默跳过（宁可不改不可改错）。
    public static func applyEditInstructions(
        text: String, rawOutput: String, hotwords: [String]
    ) -> String {
        let normalizedHotwords = Dictionary(
            hotwords.map { ($0.lowercased(), $0) }, uniquingKeysWith: { first, _ in first })
        var out = text
        var applied = 0
        for line in rawOutput.split(separator: "\n", omittingEmptySubsequences: true) {
            var line = line.trimmingCharacters(in: .whitespaces)
            line = line.trimmingCharacters(in: CharacterSet(charactersIn: "`"))
            if line.isEmpty || line == "无" || line == "处理：" || line == "处理:"
                || line.hasPrefix("输入") || line.hasPrefix("热词表") {
                continue
            }
            if applied >= maxEdits { break }
            if line.hasPrefix("删除") {
                // 删除：片段（全角/半角冒号都兼容）。
                var fragment: Substring = line.dropFirst("删除".count)
                if fragment.hasPrefix("：") { fragment = fragment.dropFirst() }
                else if fragment.hasPrefix(":") { fragment = fragment.dropFirst() }
                let frag = fragment.trimmingCharacters(in: .whitespaces)
                if frag.utf16.count > 0, frag.utf16.count <= 10,
                   let range = out.range(of: frag) {
                    out.replaceSubrange(range, with: "")
                    applied += 1
                }
                continue
            }
            if line.contains("→") {
                let parts = line.components(separatedBy: "→")
                guard parts.count >= 2 else { continue }
                let left = parts[0].trimmingCharacters(in: .whitespaces)
                    .trimmingCharacters(in: CharacterSet(charactersIn: "`"))
                var right = parts[1].trimmingCharacters(in: .whitespaces)
                    .trimmingCharacters(in: CharacterSet(charactersIn: "`"))
                right = right.trimmingCharacters(in: CharacterSet(charactersIn: "。"))
                guard let target = normalizedHotwords[right.lowercased()],
                      !left.isEmpty, left != target else { continue }
                // 大小写不敏感定位（OG→Ogg 应命中原文中的 Og）。
                if let range = out.range(of: left, options: [.caseInsensitive]) {
                    out.replaceSubrange(range, with: target)
                    applied += 1
                }
            }
        }
        return out
    }

    // MARK: - L1 确定性清理（评测 deterministic_clean v5 移植）

    private static let leadingFillerRegex: NSRegularExpression = {
        // r5 修正口径：语气词（嗯/啊/呃/哦）后跟标点或空白即剥；指示/连接词
        //（那个/这个/就是/然后呢）仅在后跟逗号时剥（无逗号视作实义——「这个API」）。
        let interjection = "(?:嗯|啊|呃|哦)[，,、]?\\s*"
        let demonstrative = "(?:(?:那个|这个|就是|然后呢)[，,])"
        let pattern = "^(?:" + interjection + "|" + demonstrative + ")+\\s*"
        return try! NSRegularExpression(pattern: pattern)
    }()
    private static let cjkRunRegex = try! NSRegularExpression(
        pattern: "([\\u4e00-\\u9fff])\\1{2,}")
    private static let cjkSpaceRegex = try! NSRegularExpression(
        pattern: "(?<=[\\u4e00-\\u9fff])[ \\t]+(?=[\\u4e00-\\u9fff])")

    /// 句首填充词剥离（≤3 轮防「嗯，啊，」级联）、3+ 连字口吃折叠、CJK 间空格移除。
    public static func deterministicClean(_ text: String) -> String {
        var result = text
        for _ in 0..<3 {
            let range = NSRange(result.startIndex..., in: result)
            let next = Self.leadingFillerRegex.stringByReplacingMatches(
                in: result, options: [], range: range, withTemplate: "")
            if next == result { break }
            result = next
        }
        var full = NSRange(result.startIndex..., in: result)
        result = Self.cjkRunRegex.stringByReplacingMatches(
            in: result, options: [], range: full, withTemplate: "$1")
        full = NSRange(result.startIndex..., in: result)
        result = Self.cjkSpaceRegex.stringByReplacingMatches(
            in: result, options: [], range: full, withTemplate: "")
        return result
    }

    // MARK: - 输出清理（评测 clean_output 移植）

    private static let thinkRegex = try! NSRegularExpression(
        pattern: "<think>[\\s\\S]*?</think>|</?think>")

    /// 剥 think 块与成对包引号（生产两端同款清理）。
    public static func cleanOutput(_ raw: String) -> String {
        var text = raw
        let range = NSRange(text.startIndex..., in: text)
        text = Self.thinkRegex.stringByReplacingMatches(
            in: text, options: [], range: range, withTemplate: "")
        text = text.trimmingCharacters(in: .whitespacesAndNewlines)
        for (left, right) in [("「", "」"), ("“", "”"), ("\"", "\""), ("'''", "'''")] {
            if text.hasPrefix(left), text.hasSuffix(right), text.count > 2 {
                text = String(text.dropFirst(left.count).dropLast(right.count))
                    .trimmingCharacters(in: .whitespacesAndNewlines)
            }
        }
        return text
    }

    // MARK: - 实例（RefinerServing 实现）

    /// 引擎懒取（模型可能下载后才就位；每次精修时再取，吃到最新缓存状态）。
    private let engineProvider: () -> LlamaCppEngine?
    private let queue = DispatchQueue(
        label: "com.voicestick.local-editform", qos: .userInitiated)

    /// - Parameter engineProvider: 返回就绪引擎或 nil（nil = 纯 L1 规则降级）。
    public init(engineProvider: @escaping () -> LlamaCppEngine?) {
        self.engineProvider = engineProvider
    }

    public func refineStream(
        _ text: String,
        promptOverride: String,
        hotwords: [String]?,
        corrections: [CorrectionPair],
        cancel: RefineCancelToken,
        onToken: @escaping (String) -> Void,
        onActivity: @escaping () -> Void,
        onComplete: @escaping (Bool, String, RefineFallback) -> Void
    ) {
        // promptOverride/corrections 为云端精修的概念（自定义提示词/用户纠正
        // few-shot）；editform 提示词形态固定（task-shape），忽略二者。
        let words = hotwords ?? []
        queue.async { [engineProvider] in
            // 引擎存活信号立即上报：失效 2.5s 首 token 预算（那是给网络挂起的
            // 预算；本地 CPU 预填 3B 可能 >2.5s 才出首块，总预算仍全程兜底）。
            onActivity()
            // L1 确定性清理（无 LLM 也保底）。
            let base = Self.deterministicClean(text)
            guard let engine = engineProvider(), !cancel.isCancelled else {
                onComplete(true, base, .none)
                return
            }
            do {
                // L2：模型只出指令行。指令流**不经 onToken 上屏**（那会把
                // 「删除：嗯」这类指令行当精修文本显示）——只发 onActivity 心跳，
                // 最终文本经 L3 守卫执行后由 onComplete 一次性给出（Windows 本地
                // 精修同为非流式完成语义）。取消即中止引擎。
                let raw = try engine.chat(
                    systemPrompt: Self.editformSystemPrompt(hotwords: words),
                    userText: base,
                    maxGenTokens: 512
                ) { _ in
                    if cancel.isCancelled { return false }
                    onActivity()
                    return true
                }
                if cancel.isCancelled { return }
                // L3：守卫执行指令。
                let refined = Self.applyEditInstructions(
                    text: base, rawOutput: Self.cleanOutput(raw), hotwords: words)
                onComplete(!refined.isEmpty, refined.isEmpty ? base : refined, .none)
            } catch {
                // 引擎失败/超预算：回退 L1 结果（Windows 同款降级语义）。
                NSLog("LocalEditform L2 failed (%@); fallback to L1", String(describing: error))
                onComplete(true, base, .none)
            }
        }
    }
}
