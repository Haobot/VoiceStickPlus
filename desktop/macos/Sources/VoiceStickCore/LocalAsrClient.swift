import Foundation

/// 本地离线 ASR 客户端：sherpa-onnx SenseVoice-Small int8。
/// 实现标准 `ASRClient` 接口——协调器经工厂切换本地/云端，音频管线零改动。
/// 与 Windows `local_asr_client_win.cc` 同构（含调度语义），双端行为对拍口径：
///
/// 数据流：`sendOggOpusChunk` 攒 Ogg 字节流（与 BLE 路径同格式），worker 线程
/// 按 600ms 节流周期对「当前全量音频」增量 Opus 解码 + SenseVoice 滚动重解码，
/// 录音期间持续触发 onPartial（空文本不上报，录音起始静音阶段保持 Listening
/// 显示）；isLast 时最终推理触发 onFinal——无新增样本则复用上次推理结果
/// （松键即出，省一次推理）。回调在 worker 线程触发（与云端客户端的网络线程
/// 语义一致，协调器侧回调已按异步设计）。
///
/// 失败语义：模型缺失/引擎初始化失败在 `start` 如实返回 false（`lastStartError`
/// 供 UI 展示），**绝不静默降级云端**（选本地的用户目的就是不出网）；
/// 音频解析错误仅 final 阶段上报（partial 阶段静默跳过——与云端网络抖动
/// 不杀会话同口径）。
///
/// 线程模型：worker 通过 `WorkerContext` 锚定（线程闭包强捕获 ctx、ctx 弱引用
/// owner）——deinit 置 shuttingDown 后无需等待线程退出，也不存在 worker 访问
/// 已释放 self 的窗口（Swift 无 Thread.join，此为无 UAF 的等价形态）。
public final class LocalAsrClient: ASRClient {

    /// partial 推理节流周期：两次推理至少间隔 600ms——首块立即解码尽快出首个
    /// partial，其后按周期轮转；单次推理超过周期时完成后立即追赶下一轮。
    static let partialInterval = TimeInterval(600) / 1000

    public var onPartial: ((String) -> Void)?
    public var onSegment: ((ASRSegment) -> Void)?
    public var onFinal: ((String) -> Void)?
    public var onError: ((String) -> Void)?
    public var onUpgradeURL: ((URL, String) -> Void)?

    /// start 失败原因（与 Windows LastStartError 同口径，设置界面共用）。
    public private(set) var lastStartError = ""

    private let modelsDir: String
    private let numThreads: Int32
    /// 实际使用的推理引擎：生产 = SherpaSenseVoiceEngine（跨会话复用），
    /// 测试 = 构造注入的假引擎（跳过模型校验与创建）。start 前后不再变更
    ///（worker 侧无锁读取安全）。
    private let engine: SenseVoiceEngine?

    /// worker 锚定上下文（线程闭包强持有；生命周期覆盖 owner）。
    private let ctx = WorkerContext()

    // MARK: - 生命周期

    /// - Parameters:
    ///   - modelsDir: SenseVoice 模型目录（含 model.int8.onnx 与 tokens.txt）。
    ///   - numThreads: 推理线程数（与 Windows 同默认 2：SenseVoice int8 短句足够）。
    ///   - engineOverride: 测试注入假引擎；非空时跳过模型目录校验与生产引擎创建。
    public init(modelsDir: String, numThreads: Int32 = 2,
                engineOverride: SenseVoiceEngine? = nil) {
        self.modelsDir = modelsDir
        self.numThreads = numThreads
        self.engine = engineOverride
    }

    deinit {
        ctx.lock()
        ctx.shuttingDown = true
        ctx.unlock()
        ctx.wake()
    }

    // MARK: - ASRClient

    public func start(options: ASRSessionOptions) -> Bool {
        lastStartError = ""

        var engine = self.engine
        if engine == nil {
            if let error = LocalAsrClient.validateModelsDir(modelsDir) {
                lastStartError = error
                return false
            }
            do {
                engine = try SherpaSenseVoiceEngine(
                    modelsDir: modelsDir, numThreads: numThreads)
            } catch {
                lastStartError = Self.describeEngineError(error)
                return false
            }
        }
        guard let engine else { return false }

        // 引擎跨会话复用；会话输入状态整体重置（丢弃上一会话/取消后的残留数据）。
        ctx.lock()
        ctx.cancelled = false
        ctx.engine = engine
        ctx.owner = self   // 回调出口桥（emit* 经 weak owner 派发）
        ctx.resetSessionLocked()
        let needWorker = !ctx.workerStarted
        ctx.workerStarted = true
        ctx.unlock()
        ctx.wake()
        if needWorker {
            let thread = Thread { [ctx] in LocalAsrClient.runWorker(ctx) }
            thread.name = "LocalAsrClient.worker"
            thread.start()
        }
        return true
    }

    public func sendOggOpusChunk(_ data: Data, isLast: Bool) {
        guard engine != nil || ctx.engine != nil else { return }
        ctx.lock()
        ctx.oggBuffer.append(data)
        ctx.oggVersion += 1
        if isLast { ctx.isLastReceived = true }
        ctx.unlock()
        // 逐块唤醒 worker：录音期间即按节流周期滚动推理产出 partial。
        ctx.wake()
    }

    /// 协议兜底：云端客户端用 finish 结束会话；本地路径的结束信号走
    /// isLast chunk，这里幂等补位（已 isLast 时无操作）。
    public func finish() {
        ctx.lock()
        let alreadyLast = ctx.isLastReceived
        if !alreadyLast { ctx.isLastReceived = true }
        ctx.unlock()
        if !alreadyLast { ctx.wake() }
    }

    public func cancel() {
        ctx.lock()
        ctx.cancelled = true
        ctx.resetSessionLocked()   // 清积压并递增 sessionId，worker 丢弃增量态
        ctx.unlock()
        ctx.wake()
        // 引擎跨会话复用不销毁；下次 start 重置取消态即可。
    }

    // MARK: - 校验（与 Windows ValidateSenseVoiceModelsDir 同口径文案）

    /// 校验模型目录是否含 model.int8.onnx 与 tokens.txt：有效返回 nil，
    /// 无效返回中文错误描述（只查存在性，不哈希——UI 线程也调用，不能冻结）。
    public static func validateModelsDir(_ modelsDir: String) -> String? {
        let fm = FileManager.default
        let model = (modelsDir as NSString).appendingPathComponent("model.int8.onnx")
        let tokens = (modelsDir as NSString).appendingPathComponent("tokens.txt")
        guard fm.fileExists(atPath: model), fm.fileExists(atPath: tokens) else {
            return "本地识别模型未就位: \(modelsDir)（需 SenseVoice model.int8.onnx 与 tokens.txt）"
        }
        return nil
    }

    private static func describeEngineError(_ error: Error) -> String {
        if case SherpaSenseVoiceEngine.EngineError.dylibNotFound(let searched) = error {
            return "本地识别运行时未就位（libsherpa-onnx-c-api.dylib），搜索过: "
                + searched.joined(separator: "; ")
                + "。可用 scripts/setup-mac-local-asr.sh 安装。"
        }
        return "本地识别引擎初始化失败（模型损坏或不受支持）"
    }

    // MARK: - worker 状态锚（线程闭包强捕获；owner 弱引用）

    final class WorkerContext {
        let cond = NSCondition()
        var shuttingDown = false
        var cancelled = false
        var workerStarted = false
        var sessionId: UInt64 = 0     // start/cancel 递增：worker 据此丢弃增量状态
        var oggVersion: UInt64 = 0    // 每次追加递增：worker 据此判断有新数据
        var isLastReceived = false
        var oggBuffer = Data()
        weak var engine: SenseVoiceEngine?
        weak var owner: LocalAsrClient?

        func lock() { cond.lock() }
        func unlock() { cond.unlock() }
        func wake() {
            cond.lock()
            cond.broadcast()
            cond.unlock()
        }

        func resetSessionLocked() {
            sessionId += 1
            oggVersion += 1
            oggBuffer.removeAll(keepingCapacity: true)
            isLastReceived = false
        }

        func isCancelledLocked() -> Bool { cancelled }

        /// worker 侧回调出口：owner 释放后自动失效（weak），线程零 self 访问。
        var emitPartial: ((String) -> Void)? { owner?.onPartial }
        var emitFinal: ((String) -> Void)? { owner?.onFinal }
        var emitError: ((String) -> Void)? { owner?.onError }

        /// 无锁快照取消位（Windows atomic 语义的等价读）。
        func snapshotCancelled() -> Bool {
            lock()
            defer { unlock() }
            return cancelled
        }
    }

    // MARK: - worker 主循环（Windows WorkerMain 的 1:1 移植）

    private static func runWorker(_ ctx: WorkerContext) {
        // worker 私有增量状态（锁外独占），随 sessionId 变化整体重置。
        var mySession: UInt64 = 0
        var myVersion: UInt64 = 0
        var sessionFinalDone = false
        var throttleArmed = false
        var nextDecodeTime = Date.distantPast
        var decoder: AudioOpusDecoder?
        var pcm = [Int16]()
        var packetsDecoded = 0
        var lastText = ""                  // 上次推理文本（final 无新样本时复用）
        var lastDecodedSamples = 0

        while true {
            var ogg = Data()
            var session: UInt64 = 0
            var finalJob = false
            var cancelledNow = false
            var engine: SenseVoiceEngine?
            ctx.lock()
            while true {
                if ctx.shuttingDown {
                    ctx.unlock()
                    return
                }
                finalJob = ctx.isLastReceived && !sessionFinalDone
                // 空 buffer 的版本变化是 start/cancel 的重置产物，不算新数据
                //（否则首个真音频块会被误武装的节流挡住 600ms）。
                let hasNew = ctx.oggVersion > myVersion && !ctx.oggBuffer.isEmpty
                // 会话首块立即解码（尽快出首个 partial），其后按节流周期。
                if finalJob || (hasNew && !throttleArmed) { break }
                if hasNew, Date() >= nextDecodeTime { break }
                // 无新数据睡到事件；有新数据睡到节流点（虚假唤醒由谓词兜底）。
                _ = ctx.cond.wait(until: hasNew ? nextDecodeTime : .distantFuture)
            }
            session = ctx.sessionId
            finalJob = ctx.isLastReceived && !sessionFinalDone
            cancelledNow = ctx.isCancelledLocked()
            engine = ctx.engine
            ogg.append(ctx.oggBuffer)
            myVersion = ctx.oggVersion
            ctx.unlock()

            if session != mySession {   // 新会话：丢弃上轮增量状态
                mySession = session
                sessionFinalDone = false
                throttleArmed = false
                decoder = try? AudioOpusDecoder()
                pcm.removeAll(keepingCapacity: true)
                packetsDecoded = 0
                lastText = ""
                lastDecodedSamples = 0
            }
            if cancelledNow { continue }   // 已取消：丢弃积压，挂起等新会话

            // 引擎由 start 保证就位；解码器创建失败按 final 错误上报防忙转。
            guard let engine else {
                if finalJob {
                    ctx.emitError?("本地识别失败：引擎不可用")
                    sessionFinalDone = true
                }
                continue
            }
            guard let decoder else {
                if finalJob {
                    ctx.emitError?("本地识别失败：Opus 解码器创建失败")
                    sessionFinalDone = true
                }
                continue
            }

            // 增量解析+解码 ogg 快照中尚未解码的 Opus packet，追加 pcm。
            var errorText: String?
            if !Self.appendNewPackets(ogg, &packetsDecoded, decoder, &pcm, &errorText) {
                if finalJob {
                    ctx.emitError?("本地识别失败：" + (errorText ?? "音频数据无法解析"))
                    sessionFinalDone = true
                } else if !throttleArmed {   // 首块即失败：武装节流防高频重试
                    throttleArmed = true
                    nextDecodeTime = Date().addingTimeInterval(partialInterval)
                }
                continue
            }
            if pcm.isEmpty {
                if finalJob {
                    ctx.emitError?("本地识别失败：无有效音频样本")
                    sessionFinalDone = true
                }
                continue
            }
            if pcm.count == lastDecodedSamples {
                // 无新音频样本：final 直接复用上次推理结果（松键即出，省一次推理）。
                if finalJob, !ctx.snapshotCancelled() {
                    ctx.emitFinal?(lastText)
                    sessionFinalDone = true
                }
                continue
            }
            let text = engine.decode(pcm)
            if ctx.snapshotCancelled() { continue }
            lastText = text
            lastDecodedSamples = pcm.count
            nextDecodeTime = Date().addingTimeInterval(partialInterval)
            throttleArmed = true
            if finalJob {
                ctx.emitFinal?(text)
                sessionFinalDone = true
            } else if !text.isEmpty {
                // 空文本 partial 不上报：录音起始静音阶段保持 Listening 显示。
                ctx.emitPartial?(text)
            }
        }
    }

    /// 增量解析+解码。返回 false = 解析/解码出错（partial 阶段调用方静默跳过；
    /// final 阶段由调用方统一报错）。
    private static func appendNewPackets(
        _ ogg: Data, _ packetsDecoded: inout Int,
        _ decoder: AudioOpusDecoder, _ pcm: inout [Int16],
        _ errorText: inout String?
    ) -> Bool {
        var stream = OggOpusStream()
        if !OggOpusDemuxer.parse(ogg, into: &stream), stream.packets.isEmpty {
            errorText = "音频数据无法解析"
            return false
        }
        if stream.packets.count <= packetsDecoded { return true }   // 无新包
        for packet in stream.packets[packetsDecoded...] {
            guard let frame = decoder.decode(packet) else {
                errorText = "Opus 解码出错"
                return false
            }
            pcm.append(contentsOf: frame)
        }
        packetsDecoded = stream.packets.count
        return true
    }
}
