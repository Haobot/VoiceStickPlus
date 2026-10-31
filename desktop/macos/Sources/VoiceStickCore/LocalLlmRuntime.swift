import Foundation

/// 内嵌 llama.cpp 引擎枢纽：档位解析（对齐 1.5B / 精修 3B 默认档 + 路径覆盖）+
/// 引擎实例缓存（按 `path#threads`，懒加载、进程内复用）。
///
/// 与 Windows `ResolveLocalRefineModelPath` 的档位语义对齐：env 覆盖 >
/// 配置路径 > 默认档（存在才用）；两功能位默认档不同（对齐 1.5B、精修 3B，
/// 各按评测最优），同模型时自然共享同一引擎实例。
public final class LocalLlmRuntime {

    public static let shared = LocalLlmRuntime()

    private let lock = NSLock()
    /// path#threads → 引擎。
    private var engines: [String: LlamaCppEngine] = [:]
    /// 加载失败的 path → (size, mtime)：文件变化（如刚下载完）后允许重试。
    private var failedLoads: [String: (size: Int64, mtime: TimeInterval)] = [:]

    private init() {}

    // MARK: - 档位解析

    /// 对齐默认档相对路径（1.5B，评测唯一推荐档）。
    public static let alignTierRelPath =
        "Qwen2.5-1.5B-Instruct-Q4_K_M/Qwen2.5-1.5B-Instruct-Q4_K_M.gguf"
    /// 精修默认档相对路径（3B，editform 评测最优）。
    public static let refineTierRelPath =
        "Qwen2.5-3B-Instruct-Q4_K_M/Qwen2.5-3B-Instruct-Q4_K_M.gguf"

    /// 解析 GGUF 绝对路径：override 非空优先（~ 展开 / 绝对 / 相对锚模型目录），
    /// 否则默认档（须存在）；env `VOICESTICK_REFINE_MODEL` 最高优先（测试用，
    /// 对齐/精修共用——同 Windows 语义）。返回 nil = 该档不可用。
    public static func modelPath(
        tierRelPath: String, modelsDir: String, override: String,
        configDirectory: URL
    ) -> String? {
        if let env = ProcessInfo.processInfo.environment["VOICESTICK_REFINE_MODEL"],
           !env.isEmpty, FileManager.default.fileExists(atPath: env) {
            return env
        }
        let fm = FileManager.default
        // 复用 resolvedModelsDir 的路径语义（~ / 相对锚配置目录）。
        var probe = LocalAsrSettings()
        probe.modelsDir = modelsDir
        let resolvedDir = probe.resolvedModelsDir(configDirectory: configDirectory)

        let trimmed = override.trimmingCharacters(in: .whitespacesAndNewlines)
        if !trimmed.isEmpty {
            let url: URL
            if trimmed.hasPrefix("~") {
                let rest = trimmed.dropFirst().droppingLeadingSlash
                url = fm.homeDirectoryForCurrentUser.appendingPathComponent(String(rest))
            } else if trimmed.hasPrefix("/") {
                url = URL(fileURLWithPath: trimmed)
            } else {
                url = URL(fileURLWithPath: resolvedDir).appendingPathComponent(trimmed)
            }
            return fm.fileExists(atPath: url.path) ? url.path : nil
        }
        let tier = URL(fileURLWithPath: resolvedDir).appendingPathComponent(tierRelPath)
        return fm.fileExists(atPath: tier.path) ? tier.path : nil
    }

    /// 对齐档路径（存在才非 nil）。
    public static func alignModelPath(
        modelsDir: String, override: String, configDirectory: URL
    ) -> String? {
        modelPath(
            tierRelPath: alignTierRelPath, modelsDir: modelsDir,
            override: override, configDirectory: configDirectory)
    }

    /// 精修档路径（存在才非 nil）。
    public static func refineModelPath(
        modelsDir: String, override: String, configDirectory: URL
    ) -> String? {
        modelPath(
            tierRelPath: refineTierRelPath, modelsDir: modelsDir,
            override: override, configDirectory: configDirectory)
    }

    // MARK: - 引擎缓存

    /// 取（或懒建）指定模型的引擎。dylib/模型缺失或加载失败返回 nil（失败按
    /// 文件 size+mtime 记忆，文件未变不重试——防反复尝试加载 2GB 模型卡 UI）。
    public func engine(modelPath: String, numThreads: Int32) -> LlamaCppEngine? {
        let key = "\(modelPath)#\(numThreads)"
        lock.lock()
        if let cached = engines[key] {
            lock.unlock()
            return cached
        }
        if let failure = failedLoads[modelPath] {
            let attrs = try? FileManager.default.attributesOfItem(atPath: modelPath)
            let size = (attrs?[.size] as? Int64) ?? -1
            let mtime = (attrs?[.modificationDate] as? Date)?.timeIntervalSince1970 ?? -1
            if size == failure.size, mtime == failure.mtime {
                lock.unlock()
                return nil   // 同一文件加载失败过：不再重试
            }
            failedLoads[modelPath] = nil
        }
        lock.unlock()

        // 模型加载在锁外（数秒级，不阻塞其他档位取用）。
        guard let engine = try? LlamaCppEngine(
            modelPath: modelPath, numThreads: numThreads) else {
            let attrs = try? FileManager.default.attributesOfItem(atPath: modelPath)
            lock.lock()
            failedLoads[modelPath] = (
                (attrs?[.size] as? Int64) ?? -1,
                (attrs?[.modificationDate] as? Date)?.timeIntervalSince1970 ?? -1)
            lock.unlock()
            NSLog("LocalLlm engine load failed: %@", modelPath)
            return nil
        }
        lock.lock()
        engines[key] = engine
        lock.unlock()
        NSLog("LocalLlm engine ready: %@", modelPath)
        return engine
    }

    /// 丢弃全部引擎（设置热更线程数/路径后由调用方决定是否重建）。
    public func resetEngines() {
        lock.lock()
        engines.removeAll()
        failedLoads.removeAll()
        lock.unlock()
    }
}

// MARK: - targeted 对齐的内嵌引擎入口

/// targeted 目标词对齐的引擎调用封装（原 LocalAlignClient 的 Ollama HTTP 路径
/// 退役，2026-10-29 起 macOS 走内嵌 llama.cpp；few-shot/输出解析复用
/// VoiceTargetedAlign 纯逻辑——评测口径不变）。
public enum LocalTargetedAlignEngine {

    /// 同步对齐：引擎贪心推理 + `VoiceTargetedAlign.parseAnswer` 解析。
    /// 返回命中候选原文；NONE/任何失败/超时 → nil（回退「普通文本粘贴」路径）。
    ///
    /// 线程契约：调用方须在后台线程（引擎推理阻塞数秒级）。超时由内部 watchdog
    /// 兜底：到点返回 nil，未完成的推理在后台自然结束（引擎内部锁串行化，
    /// 结果丢弃）——CPU 预填长文本的最坏情况不会卡死调用方。
    public static func align(
        engine: LlamaCppEngine,
        command: String,
        asrTarget: String,
        remainingText: String,
        candidates: [String],
        timeoutMS: Int
    ) -> String? {
        let semaphore = DispatchSemaphore(value: 0)
        var answer: String?
        DispatchQueue.global(qos: .userInitiated).async {
            defer { semaphore.signal() }
            guard let raw = try? engine.chat(
                systemPrompt: VoiceTargetedAlign.systemPrompt,
                fewShotTurns: VoiceTargetedAlign.fewShotTurns(),
                userText: VoiceTargetedAlign.userPrompt(
                    command: command, asrTarget: asrTarget,
                    remainingText: remainingText, candidates: candidates),
                maxGenTokens: 64,
                grammar: VoiceTargetedAlign.answerEnumGrammar(candidates: candidates)
            ) else { return }
            answer = VoiceTargetedAlign.parseAnswer(raw, candidates: candidates)
        }
        let waitResult = semaphore.wait(timeout: .now() + .milliseconds(max(100, timeoutMS)))
        if waitResult == .timedOut { return nil }
        guard let answer, answer != "NONE" else { return nil }
        return answer
    }
}
