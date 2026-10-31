import CLlamaCpp
import Foundation

/// 内嵌 llama.cpp 推理引擎（Windows `llama_cpp_engine.cc` 的 Swift 移植，pin b10868）。
///
/// 语义与 Windows 单轮 Chat 1:1：ChatML 手拼模板、few-shot system 前缀的 **KV 复用**
/// （前缀命中只清上一句 user+生成段，评测 70ms 级延迟的前提）、贪心采样、
/// UTF-8 边界流式回调。跨轮会话（ChatSessionTurn）为 Windows cross_turn 专属特性，
/// 本里程碑不移植。
///
/// **运行时绑定（与 SherpaSenseVoiceEngine 同模式）**：C 结构体类型经
/// `Sources/CLlamaCpp`（header-only，入库）编译期可见；函数符号运行时 dlopen
/// `libvoicestick-llama.dylib`（由 scripts/fetch-llama-cpp.sh 把 llama+ggml 静态库
/// 合成单 dylib 安装），仓库无 dylib 也能构建、无 dylib 时如实报错不崩。
/// dylib 搜索序：env `VOICESTICK_LLAMA_DIR` → app bundle Frameworks →
/// `~/Library/Application Support/VoiceStick/models/llama/lib`。
///
/// 与 Windows 的两处刻意差异：
/// 1. assistant 段起始的空 `<think>` 块改为**可选**（Windows 硬编码给 Qwen3）——
///    我们的模型是 Qwen2.5（无思考位，Ollama 评测口径即纯 ChatML），注入空 think
///    属于分布外输入会劣化输出；Qwen3 系才需要 `includeEmptyThinkBlock: true`。
/// 2. CPU 线程默认 4（Windows 默认 6 为 x86 大核；M 系性能/能效核混合取 4 稳妥）。
public final class LlamaCppEngine {

    public enum EngineError: LocalizedError {
        case dylibNotFound(searched: [String])
        case symbolMissing(String)
        case modelLoadFailed(String)
        case contextCreateFailed
        case tokenizeFailed
        /// 输入超出上下文预算（2048 token）。
        case contextOverflow
        case inferenceFailed(String)

        public var errorDescription: String? {
            switch self {
            case .dylibNotFound(let searched):
                return "本地 LLM 运行时未就位（libvoicestick-llama.dylib），搜索过: "
                    + searched.joined(separator: "; ")
                    + "。可用 scripts/fetch-llama-cpp.sh 安装。"
            case .symbolMissing(let symbol):
                return "本地 LLM 运行时缺符号：\(symbol)（dylib 版本与 pin 不符？）"
            case .modelLoadFailed(let path):
                return "本地 LLM 模型加载失败：\(path)"
            case .contextCreateFailed:
                return "本地 LLM 推理上下文创建失败"
            case .tokenizeFailed:
                return "本地 LLM 分词失败"
            case .contextOverflow:
                return "本地 LLM 输入超上下文预算"
            case .inferenceFailed(let detail):
                return "本地 LLM 推理失败：\(detail)"
            }
        }
    }

    /// dylib 文件名（fetch-llama-cpp.sh 的合成产物）。
    static let libraryName = "libvoicestick-llama.dylib"

    // ChatML 模板（Qwen 系官方 transformers 模板等价手拼；Windows 同款常量）。
    private static let chatmlSystemHeader = "<|im_start|>system\n"
    private static let chatmlUserHeader = "<|im_end|>\n<|im_start|>user\n"
    /// Qwen3 关思考位的标准做法（assistant 段起始注入空 think 块）；Qwen2.5 不用。
    private static let chatmlAssistantHeaderPlain = "<|im_end|>\n<|im_start|>assistant\n"
    private static let chatmlAssistantHeaderThink =
        "<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n"

    /// 上下文预算：few-shot system 前缀(~800) + user(≤512) + 生成上限(≤512)。
    private static let ctxTokens: Int32 = 2048

    // MARK: - dlopen 绑定

    private var handle: UnsafeMutableRawPointer?

    private var backendInit: (@convention(c) () -> Void)!
    private var modelDefaultParams: (@convention(c) () -> llama_model_params)!
    private var modelLoadFromFile:
        (@convention(c) (UnsafePointer<CChar>?, llama_model_params) -> OpaquePointer?)!
    private var modelFree: (@convention(c) (OpaquePointer?) -> Void)!
    private var contextDefaultParams: (@convention(c) () -> llama_context_params)!
    private var initFromModel:
        (@convention(c) (OpaquePointer?, llama_context_params) -> OpaquePointer?)!
    private var freeContext: (@convention(c) (OpaquePointer?) -> Void)!
    private var modelGetVocab:
        (@convention(c) (OpaquePointer?) -> OpaquePointer?)!
    private var tokenize: (@convention(c) (
        OpaquePointer?, UnsafePointer<CChar>?, Int32,
        UnsafeMutablePointer<llama_token>?, Int32, Bool, Bool) -> Int32)!
    private var getMemory: (@convention(c) (OpaquePointer?) -> OpaquePointer?)!
    private var memorySeqRm: (@convention(c) (
        OpaquePointer?, llama_seq_id, llama_pos, llama_pos) -> Bool)!
    private var batchInit: (@convention(c) (Int32, Int32, Int32) -> llama_batch)!
    private var batchFree: (@convention(c) (UnsafeMutablePointer<llama_batch>?) -> Void)!
    private var decode: (@convention(c) (
        OpaquePointer?, llama_batch) -> Int32)!
    private var samplerChainDefaultParams: (@convention(c) () -> llama_sampler_chain_params)!
    private var samplerChainInit:
        (@convention(c) (llama_sampler_chain_params) -> OpaquePointer?)!
    private var samplerChainAdd: (@convention(c) (
        OpaquePointer?, OpaquePointer?) -> Void)!
    private var samplerInitGreedy: (@convention(c) () -> OpaquePointer?)!
    private var samplerFree: (@convention(c) (OpaquePointer?) -> Void)!
    private var samplerSample: (@convention(c) (
        OpaquePointer?, OpaquePointer?, Int32) -> llama_token)!
    private var samplerInitGrammar: (@convention(c) (
        OpaquePointer?, UnsafePointer<CChar>?, UnsafePointer<CChar>?) -> OpaquePointer?)!
    private var vocabIsEog: (@convention(c) (OpaquePointer?, llama_token) -> Bool)!
    private var tokenToPiece: (@convention(c) (
        OpaquePointer?, llama_token, UnsafeMutablePointer<CChar>?, Int32, Int32, Bool) -> Int32)!

    /// dylib 搜索路径（诊断输出用；顺序见类注释）。
    public static func librarySearchPaths() -> [String] {
        var paths: [String] = []
        if let env = ProcessInfo.processInfo.environment["VOICESTICK_LLAMA_DIR"], !env.isEmpty {
            paths.append(env + "/lib/" + libraryName)
        }
        paths.append(Bundle.main.bundleURL.path + "/Contents/Frameworks/" + libraryName)
        if let appSupport = FileManager.default.urls(
            for: .applicationSupportDirectory, in: .userDomainMask).first {
            paths.append(appSupport.appendingPathComponent(
                "VoiceStick/models/llama/lib/" + libraryName).path)
        }
        return paths
    }

    // MARK: - 实例状态

    private var model: OpaquePointer?
    private var context: OpaquePointer?
    private var vocab: OpaquePointer?
    private var maxGenTokensDefault = 256
    private var cachedPrefix = ""
    private var prefixLen = 0
    /// 引擎调用串行化（精修 worker 与对齐路径可能并发触达同一实例）。
    private let lock = NSLock()

    /// - Parameters:
    ///   - modelPath: GGUF 模型文件路径。
    ///   - numThreads: CPU 推理线程数。
    ///   - maxGenTokens: 默认生成上限（chat 可逐调用覆盖）。
    ///   - libraryDir: 显式 dylib 所在目录根（含 lib/）；nil = 按搜索序。
    public init(modelPath: String, numThreads: Int32 = 4, maxGenTokens: Int = 256,
                libraryDir: String? = nil) throws {
        // 1) dlopen。
        let dylibPath: String
        if let libraryDir {
            dylibPath = libraryDir + "/lib/" + Self.libraryName
        } else {
            dylibPath = Self.librarySearchPaths().first { FileManager.default.fileExists(atPath: $0) }
                ?? (Self.librarySearchPaths().first ?? "")
        }
        guard let handle = dlopen(dylibPath, RTLD_NOW | RTLD_LOCAL) else {
            throw EngineError.dylibNotFound(searched: Self.librarySearchPaths())
        }
        self.handle = handle

        // 2) dlsym 绑定（逐个检查，任一缺失即失败——半绑定引擎不可用）。
        var missing: String?
        func bind(_ name: String) -> UnsafeMutableRawPointer? {
            if let p = dlsym(handle, name) { return p }
            if missing == nil { missing = name }
            return nil
        }

        if let p = bind("llama_backend_init") {
            backendInit = unsafeBitCast(p, to: (@convention(c) () -> Void).self)
        }
        if let p = bind("llama_model_default_params") {
            modelDefaultParams = unsafeBitCast(p, to: (@convention(c) () -> llama_model_params).self)
        }
        if let p = bind("llama_model_load_from_file") {
            modelLoadFromFile = unsafeBitCast(p, to: (@convention(c) (
                UnsafePointer<CChar>?, llama_model_params) -> OpaquePointer?).self)
        }
        if let p = bind("llama_model_free") {
            modelFree = unsafeBitCast(p, to: (@convention(c) (OpaquePointer?) -> Void).self)
        }
        if let p = bind("llama_context_default_params") {
            contextDefaultParams = unsafeBitCast(p, to: (@convention(c) () -> llama_context_params).self)
        }
        if let p = bind("llama_init_from_model") {
            initFromModel = unsafeBitCast(p, to: (@convention(c) (
                OpaquePointer?, llama_context_params) -> OpaquePointer?).self)
        }
        if let p = bind("llama_free") {
            freeContext = unsafeBitCast(p, to: (@convention(c) (OpaquePointer?) -> Void).self)
        }
        if let p = bind("llama_model_get_vocab") {
            modelGetVocab = unsafeBitCast(p, to: (@convention(c) (
                OpaquePointer?) -> OpaquePointer?).self)
        }
        if let p = bind("llama_tokenize") {
            tokenize = unsafeBitCast(p, to: (@convention(c) (
                OpaquePointer?, UnsafePointer<CChar>?, Int32,
                UnsafeMutablePointer<llama_token>?, Int32, Bool, Bool) -> Int32).self)
        }
        if let p = bind("llama_get_memory") {
            getMemory = unsafeBitCast(p, to: (@convention(c) (
                OpaquePointer?) -> OpaquePointer?).self)
        }
        if let p = bind("llama_memory_seq_rm") {
            memorySeqRm = unsafeBitCast(p, to: (@convention(c) (
                OpaquePointer?, llama_seq_id, llama_pos, llama_pos) -> Bool).self)
        }
        if let p = bind("llama_batch_init") {
            batchInit = unsafeBitCast(p, to: (@convention(c) (
                Int32, Int32, Int32) -> llama_batch).self)
        }
        if let p = bind("llama_batch_free") {
            batchFree = unsafeBitCast(p, to: (@convention(c) (
                UnsafeMutablePointer<llama_batch>?) -> Void).self)
        }
        if let p = bind("llama_decode") {
            decode = unsafeBitCast(p, to: (@convention(c) (
                OpaquePointer?, llama_batch) -> Int32).self)
        }
        if let p = bind("llama_sampler_chain_default_params") {
            samplerChainDefaultParams = unsafeBitCast(
                p, to: (@convention(c) () -> llama_sampler_chain_params).self)
        }
        if let p = bind("llama_sampler_chain_init") {
            samplerChainInit = unsafeBitCast(p, to: (@convention(c) (
                llama_sampler_chain_params) -> OpaquePointer?).self)
        }
        if let p = bind("llama_sampler_chain_add") {
            samplerChainAdd = unsafeBitCast(p, to: (@convention(c) (
                OpaquePointer?, OpaquePointer?) -> Void).self)
        }
        if let p = bind("llama_sampler_init_greedy") {
            samplerInitGreedy = unsafeBitCast(p, to: (@convention(c) () -> OpaquePointer?).self)
        }
        if let p = bind("llama_sampler_free") {
            samplerFree = unsafeBitCast(p, to: (@convention(c) (OpaquePointer?) -> Void).self)
        }
        if let p = bind("llama_sampler_sample") {
            samplerSample = unsafeBitCast(p, to: (@convention(c) (
                OpaquePointer?, OpaquePointer?, Int32) -> llama_token).self)
        }
        if let p = bind("llama_sampler_init_grammar") {
            samplerInitGrammar = unsafeBitCast(p, to: (@convention(c) (
                OpaquePointer?, UnsafePointer<CChar>?,
                UnsafePointer<CChar>?) -> OpaquePointer?).self)
        }
        if let p = bind("llama_vocab_is_eog") {
            vocabIsEog = unsafeBitCast(p, to: (@convention(c) (
                OpaquePointer?, llama_token) -> Bool).self)
        }
        if let p = bind("llama_token_to_piece") {
            tokenToPiece = unsafeBitCast(p, to: (@convention(c) (
                OpaquePointer?, llama_token, UnsafeMutablePointer<CChar>?, Int32,
                Int32, Bool) -> Int32).self)
        }
        if let missing {
            throw EngineError.symbolMissing(missing)
        }

        // 3) 加载模型 + 建上下文。
        backendInit()   // CPU-only 幂等
        let modelParams = modelDefaultParams()
        let modelPathCString = Array(modelPath.utf8CString)
        guard let model = modelPathCString.withUnsafeBufferPointer({ ptr in
            modelLoadFromFile(ptr.baseAddress, modelParams)
        }) else {
            throw EngineError.modelLoadFailed(modelPath)
        }
        self.model = model

        var contextParams = contextDefaultParams()
        contextParams.n_ctx = UInt32(Self.ctxTokens)
        contextParams.n_seq_max = 1
        contextParams.n_threads = Int32(max(1, numThreads))
        contextParams.n_threads_batch = Int32(max(1, numThreads))
        guard let context = initFromModel(model, contextParams) else {
            modelFree(model)
            self.model = nil
            throw EngineError.contextCreateFailed
        }
        self.context = context
        self.vocab = modelGetVocab(model)
        self.maxGenTokensDefault = maxGenTokens > 0 ? maxGenTokens : 256
    }

    deinit {
        if let context { freeContext(context) }
        if let model { modelFree(model) }
        // handle 不 dlclose：进程生命周期内可能仍被其他引擎实例引用。
    }

    // MARK: - 对话

    /// 单轮对话（无会话语义）。返回完整生成文本；onToken 按 UTF-8 边界增量回调
    ///（返回 false 中止）。前缀 KV 复用：system + fewShotTurns 不变时只重算
    /// user+生成段（few-shot 属于静态前缀，跨调用复用——评测延迟口径的前提）。
    public func chat(
        systemPrompt: String,
        fewShotTurns: [(user: String, assistant: String)] = [],
        userText: String,
        includeEmptyThinkBlock: Bool = false,
        maxGenTokens: Int? = nil,
        grammar: String? = nil,
        onToken: ((String) -> Bool)? = nil
    ) throws -> String {
        lock.lock()
        defer { lock.unlock() }
        guard let context, let vocab else { throw EngineError.contextCreateFailed }

        let assistantHeader = includeEmptyThinkBlock
            ? Self.chatmlAssistantHeaderThink : Self.chatmlAssistantHeaderPlain
        // 前缀 = system + few-shot 轮（静态，跨调用 KV 复用）。
        var prefix = Self.chatmlSystemHeader + systemPrompt
        for turn in fewShotTurns {
            prefix += Self.chatmlUserHeader + turn.user + assistantHeader + turn.assistant
        }
        let memory = getMemory(context)

        if cachedPrefix != prefix {
            // 前缀变化（如热词段接入）：全量重建前缀 KV。
            memorySeqRm(memory, 0, 0, -1)
            let prefixTokens = try tokenizeLocked(vocab, prefix)
            guard !prefixTokens.isEmpty else { throw EngineError.tokenizeFailed }
            guard decodeBatchLocked(context, prefixTokens, 0) else {
                throw EngineError.inferenceFailed("prefix decode")
            }
            cachedPrefix = prefix
            prefixLen = prefixTokens.count
        } else {
            // 前缀命中：只清上一句的 user+生成 KV，保住前缀（复用关键路径）。
            memorySeqRm(memory, 0, Int32(prefixLen), -1)
        }

        let genBudget = maxGenTokens ?? maxGenTokensDefault
        let suffix = userText + assistantHeader
        let userTokens = try tokenizeLocked(vocab, suffix)
        guard !userTokens.isEmpty else { throw EngineError.tokenizeFailed }
        if prefixLen + userTokens.count + genBudget > Int(Self.ctxTokens) {
            throw EngineError.contextOverflow   // 异常长输入，交调用方回退
        }
        guard decodeBatchLocked(context, userTokens, Int32(prefixLen)) else {
            throw EngineError.inferenceFailed("user decode")
        }

        let samplerParams = samplerChainDefaultParams()
        let sampler = samplerChainInit(samplerParams)
        defer { samplerFree(sampler) }
        // 语法约束先入链（遮蔽非法 token），贪心后入链（合法集内 argmax）——
        // Ollama format JSON-schema 约束解码的等价形态。
        if let grammar {
            let grammarCString = Array(grammar.utf8CString)
            let rootCString = Array("root".utf8CString)
            let grammarSampler = grammarCString.withUnsafeBufferPointer { gbuf in
                rootCString.withUnsafeBufferPointer { rbuf in
                    samplerInitGrammar(vocab, gbuf.baseAddress, rbuf.baseAddress)
                }
            }
            if let grammarSampler {
                samplerChainAdd(sampler, grammarSampler)
            } else {
                throw EngineError.inferenceFailed("grammar 解析失败")
            }
        }
        samplerChainAdd(sampler, samplerInitGreedy())

        var output = [UInt8]()
        var pending = [UInt8]()   // 流式 UTF-8 边界缓冲
        var cancelled = false
        let genPosBase = Int32(prefixLen + userTokens.count)
        var token = samplerSample(sampler, context, -1)
        var generated = 0
        while generated < genBudget && !vocabIsEog(vocab, token) {
            var pieceBuffer = [CChar](repeating: 0, count: 256)
            let n = tokenToPiece(vocab, token, &pieceBuffer, Int32(pieceBuffer.count), 0, false)
            if n > 0 {
                let bytes = pieceBuffer.prefix(Int(n)).map { UInt8(bitPattern: $0) }
                output.append(contentsOf: bytes)
                if let onToken {
                    pending.append(contentsOf: bytes)
                    if Self.endsOnUTF8Boundary(pending) {
                        let chunk = String(decoding: pending, as: UTF8.self)
                        if !onToken(chunk) {
                            cancelled = true
                            break
                        }
                        pending.removeAll(keepingCapacity: true)
                    }
                }
            }
            guard decodeOneLocked(context, token, genPosBase + Int32(generated)) else {
                throw EngineError.inferenceFailed("generate decode")
            }
            generated += 1
            token = samplerSample(sampler, context, -1)
        }
        if cancelled { throw EngineError.inferenceFailed("cancelled") }
        if let onToken, !pending.isEmpty {
            onToken(String(decoding: pending, as: UTF8.self))
        }
        return String(decoding: output, as: UTF8.self)
    }

    /// 丢弃缓存前缀（提示词变化由 chat 自动检测，本方法供热更/低内存场景显式调用）。
    public func resetPrefixCache() {
        lock.lock()
        defer { lock.unlock() }
        cachedPrefix = ""
        prefixLen = 0
    }

    /// 运行时 dylib 是否就位（存在性检查，设置状态行用）。
    public static func runtimeDylibAvailable() -> Bool {
        librarySearchPaths().contains { FileManager.default.fileExists(atPath: $0) }
    }

    // MARK: - C 辅助（实例方法：经 dlsym 函数指针；调用方已持锁）

    private func tokenizeLocked(_ vocab: OpaquePointer, _ text: String) throws -> [llama_token] {
        let bytes = Array(text.utf8CString)
        let need = bytes.withUnsafeBufferPointer { ptr in
            tokenize(vocab, ptr.baseAddress, Int32(bytes.count - 1), nil, 0, false, true)
        }
        if need == 0 { return [] }
        let capacity = need < 0 ? -need : need
        var tokens = [llama_token](repeating: 0, count: Int(capacity))
        let n = tokens.withUnsafeMutableBufferPointer { tokenPtr in
            bytes.withUnsafeBufferPointer { bytePtr in
                tokenize(vocab, bytePtr.baseAddress, Int32(bytes.count - 1),
                         tokenPtr.baseAddress, Int32(tokenPtr.count), false, true)
            }
        }
        guard n > 0 else { throw EngineError.tokenizeFailed }
        tokens.removeLast(tokens.count - Int(n))
        return tokens
    }

    private func decodeBatchLocked(
        _ context: OpaquePointer, _ tokens: [llama_token], _ posBase: Int32
    ) -> Bool {
        var batch = batchInit(Int32(tokens.count), 0, 1)
        defer { batchFree(&batch) }
        for (index, token) in tokens.enumerated() {
            batch.token[index] = token
            batch.pos[index] = posBase + Int32(index)
            batch.n_seq_id[index] = 1
            batch.seq_id![index]![0] = 0
            batch.logits[index] = index == tokens.count - 1 ? 1 : 0
        }
        batch.n_tokens = Int32(tokens.count)
        return decode(context, batch) == 0
    }

    private func decodeOneLocked(
        _ context: OpaquePointer, _ token: llama_token, _ pos: Int32
    ) -> Bool {
        var batch = batchInit(1, 0, 1)
        defer { batchFree(&batch) }
        batch.token![0] = token
        batch.pos![0] = pos
        batch.n_seq_id![0] = 1
        batch.seq_id![0]![0] = 0
        batch.logits![0] = 1
        batch.n_tokens = 1
        return decode(context, batch) == 0
    }

    /// s 是否结束在完整 UTF-8 序列边界（Windows EndsOnUtf8Boundary 同判据：
    /// 末序列续字节数必须恰好等于前导字节声明的长度减一）。
    static func endsOnUTF8Boundary(_ s: [UInt8]) -> Bool {
        guard !s.isEmpty else { return true }
        var i = s.count - 1
        var cont = 0
        while true {
            let b = s[i]
            if b < 0x80 { return cont == 0 }
            if b & 0xC0 == 0xC0 {
                let length: Int
                if b & 0xE0 == 0xC0 { length = 2 }
                else if b & 0xF0 == 0xE0 { length = 3 }
                else if b & 0xF8 == 0xF0 { length = 4 }
                else { return false }   // 非法前导字节（0xF8..0xFF）
                return cont == length - 1
            }
            cont += 1
            if cont > 3 || i == 0 { return false }
            i -= 1
        }
    }
}
