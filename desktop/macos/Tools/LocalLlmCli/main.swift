// 本地 LLM 引擎复测 CLI（内嵌 llama.cpp 路径；评测口径对拍工具）。
//
// 三种模式（<model_path> 为 GGUF 绝对路径；[threads] 默认 4）：
//   chat <model_path> [threads]            —— 引擎冒烟：stdin 一行用户文本，
//                                             trivial system prompt，输出回复+延迟
//                                             （两次调用对比可见 KV 前缀复用收益）。
//   refine <model_path> <inputs.json> [threads]
//                                          —— det+editform 全管线复测（与评测
//                                             harness run_refine_bench.py 的
//                                             det+editform 臂逐字节同口径）：
//                                             输入 = harness 导出的
//                                             {inputs:[{id,asr_text,reference}],
//                                              baseline_cer, hotwords}，
//                                             输出 JSON（stdout）：逐例
//                                             base/raw/refined/latency + 汇总。
//   align <model_path> <dataset.json> [threads]
//                                          —— targeted 对齐复测：dataset.json 为
//                                             scripts/local_align_bench/dataset.json
//                                             同构 {cases:[{id,command,target,
//                                             remaining,expected}]}，逐例
//                                             candidates→引擎→解析，输出逐例+汇总。
//
// 构建：swift build -c release --product LocalLlmCli（需先 scripts/fetch-llama-cpp.sh）。
import Foundation
import VoiceStickCore

@main
struct LocalLlmCli {
    static func main() {
        let args = CommandLine.arguments
        guard args.count >= 3 else {
            FileHandle.standardError.write(Data("""
            用法:
              LocalLlmCli chat <model_path> [threads]
              LocalLlmCli refine <model_path> <inputs.json> [threads]
              LocalLlmCli align <model_path> <dataset.json> [threads]
            """.utf8))
            exit(2)
        }
        let mode = args[1]
        let modelPath = args[2]
        let threads: Int32 = args.count > 4 ? (Int32(args[4]) ?? 4) : 4

        func makeEngine() -> LlamaCppEngine? {
            try? LlamaCppEngine(modelPath: modelPath, numThreads: threads)
        }
        guard let engine = makeEngine() else {
            FileHandle.standardError.write(Data("引擎初始化失败：\(modelPath)\n".utf8))
            exit(1)
        }
        // 每例新建引擎（诊断 KV 复用路径用；模型加载 ~1s/例）。
        let freshPerCase = ProcessInfo.processInfo.environment["VOICESTICK_CLI_FRESH"] == "1"

        switch mode {
        case "chat": runChat(engine)
        case "refine":
            guard args.count >= 4 else { exit(2) }
            if freshPerCase {
                runRefine(engine, inputsPath: args[3], makeEngine: makeEngine)
            } else {
                runRefine(engine, inputsPath: args[3])
            }
        case "align":
            guard args.count >= 4 else { exit(2) }
            if freshPerCase {
                runAlign(engine, datasetPath: args[3], makeEngine: makeEngine)
            } else {
                runAlign(engine, datasetPath: args[3])
            }
        default:
            FileHandle.standardError.write(Data("未知模式：\(mode)\n".utf8))
            exit(2)
        }
    }

    // MARK: - chat 冒烟

    static func runChat(_ engine: LlamaCppEngine) {
        let input = String(data: FileHandle.standardInput.readDataToEndOfFile(), encoding: .utf8)?
            .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        guard !input.isEmpty else {
            FileHandle.standardError.write(Data("stdin 需要一行用户文本\n".utf8))
            exit(2)
        }
        for round in 1...2 {
            let t0 = Date()
            do {
                let reply = try engine.chat(
                    systemPrompt: "你是简洁的中文助手，用一句话回答。",
                    userText: input)
                let ms = Int(Date().timeIntervalSince(t0) * 1000)
                print("round\(round) [\(ms)ms] \(reply)")
            } catch {
                print("round\(round) 失败: \(error.localizedDescription)")
            }
        }
    }

    // MARK: - refine 复测（det+editform）

    struct RefineInputs: Decodable {
        struct Item: Decodable {
            let id: String
            let asr_text: String
            let reference: String
        }
        let inputs: [Item]
        let baseline_cer: [String: Double]?
        let hotwords: [String]?
    }

    static func runRefine(_ engine: LlamaCppEngine, inputsPath: String,
                          makeEngine: (() -> LlamaCppEngine?)? = nil) {
        guard let data = try? Data(contentsOf: URL(fileURLWithPath: inputsPath)),
              let payload = try? JSONDecoder().decode(RefineInputs.self, from: data) else {
            FileHandle.standardError.write(Data("输入文件解析失败：\(inputsPath)\n".utf8))
            exit(1)
        }
        let hotwords = payload.hotwords ?? []
        var rows: [[String: Any]] = []
        var caseIndex = 0
        for item in payload.inputs {
            var engine = engine
            if let makeEngine, caseIndex > 0, let fresh = makeEngine() { engine = fresh }
            caseIndex += 1
            let base = LocalEditformRefiner.deterministicClean(item.asr_text)
            let t0 = Date()
            var raw = ""
            var failed = false
            do {
                raw = try engine.chat(
                    systemPrompt: LocalEditformRefiner.editformSystemPrompt(hotwords: hotwords),
                    userText: base, maxGenTokens: 512)
            } catch {
                failed = true
            }
            let ms = Int(Date().timeIntervalSince(t0) * 1000)
            let refined = failed
                ? base
                : LocalEditformRefiner.applyEditInstructions(
                    text: base, rawOutput: LocalEditformRefiner.cleanOutput(raw),
                    hotwords: hotwords)
            rows.append([
                "id": item.id,
                "raw_asr": item.asr_text,
                "base": base,
                "model_raw": failed ? nil : raw,
                "refined": refined,
                "reference": item.reference,
                "latency_ms": ms,
                "parse_fail": failed,
            ])
        }
        let latencies = rows.compactMap { $0["latency_ms"] as? Int }
        let sorted = latencies.sorted()
        let result: [String: Any] = [
            "engine": "embedded-llama.cpp-b10868",
            "pipeline": "det_v5+editform",
            "threads": 4,
            "n": rows.count,
            "parse_fail": rows.filter { ($0["parse_fail"] as? Bool) == true }.count,
            "latency_p50_ms": sorted.isEmpty ? 0 : sorted[sorted.count / 2],
            "latency_p95_ms": sorted.isEmpty ? 0 : sorted[min(sorted.count - 1, Int(0.95 * Double(sorted.count - 1)))],
            "cases": rows,
        ]
        let json = (try? JSONSerialization.data(
            withJSONObject: result, options: [.prettyPrinted])) ?? Data()
        FileHandle.standardOutput.write(json)
    }

    // MARK: - align 复测

    struct AlignDataset: Decodable {
        struct Case: Decodable {
            let id: String
            let command: String
            let target: String
            let remaining: String
            let expected: String
        }
        let cases: [Case]
    }

    static func runAlign(_ engine: LlamaCppEngine, datasetPath: String,
                           makeEngine: (() -> LlamaCppEngine?)? = nil) {
        guard let data = try? Data(contentsOf: URL(fileURLWithPath: datasetPath)),
              let dataset = try? JSONDecoder().decode(AlignDataset.self, from: data) else {
            FileHandle.standardError.write(Data("数据集解析失败：\(datasetPath)\n".utf8))
            exit(1)
        }
        var rows: [[String: Any]] = []
        var caseIndex = 0
        for c in dataset.cases {
            var engine = engine
            if let makeEngine, caseIndex > 0, let fresh = makeEngine() { engine = fresh }
            caseIndex += 1
            let cands = VoiceTargetedAlign.candidates(asrTarget: c.target, remainingText: c.remaining)
            let texts = cands.map { $0.text }
            var answer: String? = nil
            var raw = ""
            var ms = 0
            if !texts.isEmpty {
                let t0 = Date()
                if let output = try? engine.chat(
                    systemPrompt: VoiceTargetedAlign.systemPrompt,
                    fewShotTurns: VoiceTargetedAlign.fewShotTurns(),
                    userText: VoiceTargetedAlign.userPrompt(
                        command: c.command, asrTarget: c.target,
                        remainingText: c.remaining, candidates: texts),
                    maxGenTokens: 64,
                    grammar: VoiceTargetedAlign.answerEnumGrammar(candidates: texts)) {
                    raw = output
                    answer = VoiceTargetedAlign.parseAnswer(output, candidates: texts)
                }
                ms = Int(Date().timeIntervalSince(t0) * 1000)
            }
            let effective = answer == nil || answer == "NONE" ? "NONE" : (answer ?? "NONE")
            rows.append([
                "id": c.id,
                "expected": c.expected,
                "answer": effective,
                "model_raw": raw,
                "candidates": texts,
                "hit": effective == c.expected,
                "dangerous": effective != "NONE" && effective != c.expected,
                "latency_ms": ms,
            ])
        }
        let hits = rows.filter { ($0["hit"] as? Bool) == true }.count
        let dangerous = rows.filter { ($0["dangerous"] as? Bool) == true }.count
        let latencies = rows.compactMap { $0["latency_ms"] as? Int }.sorted()
        let result: [String: Any] = [
            "engine": "embedded-llama.cpp-b10868",
            "n": rows.count,
            "hit": hits,
            "accuracy": Double(hits) / Double(max(1, rows.count)),
            "dangerous_picks": dangerous,
            "latency_p50_ms": latencies.isEmpty ? 0 : latencies[latencies.count / 2],
            "cases": rows,
        ]
        let json = (try? JSONSerialization.data(
            withJSONObject: result, options: [.prettyPrinted])) ?? Data()
        FileHandle.standardOutput.write(json)
    }
}
