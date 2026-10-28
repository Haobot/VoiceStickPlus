import Foundation
import VoiceStickCore

/// 本地 ASR 链路单测（无模型纪律：引擎一律假注入，不 mock 真实推理）：
/// ① OggOpusDemuxer ↔ OggOpusMuxer 往返（自家 muxer 产的流解析回原 packets）；
/// ② AudioOpusEncoder ↔ AudioOpusDecoder 回环 SNR；
/// ③ LocalAsrClient 状态机（假引擎驱动）：start 校验/首块 partial/空文本抑制/
///    final 复用/取消丢弃/垃圾音频 final 报错；
/// ④ [local_asr] 配置解析与目录解析；
/// ⑤ validateModelsDir。
/// 真实 SenseVoice 推理不进单测（模型 239MB 不随仓；真机验证见
/// Doc/Test/local-fullstack-eval-2026-10-28.md 复现工具）。
func runLocalAsrTests() {
    // ---- 测试工具 ----
    func waitUntil(_ timeout: TimeInterval = 3, _ pred: () -> Bool) -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if pred() { return true }
            Thread.sleep(forTimeInterval: 0.02)
        }
        return pred()
    }

    /// 生成 640 采样测试 PCM（1kHz 正弦，幅度 12000）。
    func sinePCM() -> [Int16] {
        (0..<640).map { Int16(12000.0 * sin(Double($0) * 2 * .pi / 160.0)) }
    }

    /// muxer 产真实 Ogg Opus 分块序列（与 BLE 路径同格式同节奏：每次 append
    /// 增量返回该帧页，finish 返回 EOS 尾页；chunks[0] 含头页+标签页+首帧）。
    /// 返回 (分块数组, 完整流)。字节对半切会截断页破坏解析——分块即页边界。
    func makeOggChunks(frames: Int) throws -> (chunks: [Data], ogg: Data) {
        let encoder = try AudioOpusEncoder()
        let muxer = OggOpusMuxer(sampleRate: 16000, channels: 1)
        let pcm = sinePCM()
        var chunks: [Data] = []
        for _ in 0..<frames {
            if let packet = encoder.encode(pcm) {
                chunks.append(muxer.append(opusPayload: packet, isLast: false))
            }
        }
        chunks.append(muxer.finish())
        return (chunks, chunks.reduce(Data(), +))
    }

    // ============================================================
    // ① OggOpusDemuxer 往返
    // ============================================================
    do {
        let (_, ogg) = try makeOggChunks(frames: 3)
        var stream = OggOpusStream()
        check(OggOpusDemuxer.parse(ogg, into: &stream), "LAsr: 自家 muxer 流解析成功")
        check(stream.packets.count == 3, "LAsr: 3 帧解析出 3 packets")
        check(stream.sampleRate == 16000, "LAsr: OpusHead 采样率 16000")
        check(stream.channels == 1, "LAsr: OpusHead 单声道")
        check(!OggOpusDemuxer.parse(Data("not ogg".utf8), into: &stream),
              "LAsr: 垃圾字节解析失败（如实报错）")
        // 增量兼容：半个流的已解析 packets 保留（partial 阶段滚动解码依赖）。
        var partial = OggOpusStream()
        _ = OggOpusDemuxer.parse(ogg.prefix(ogg.count / 2), into: &partial)
        check(partial.packets.count >= 1, "LAsr: 截断流保留已解析 packets")
    } catch {
        check(false, "LAsr: muxer/encoder 构造失败 \(error)")
    }

    // ============================================================
    // ② 编解码回环 SNR
    // ============================================================
    do {
        let encoder = try AudioOpusEncoder()
        let decoder = try AudioOpusDecoder()
        // 多帧连续回环 + 最佳滞后对齐（codec 固有时延，口径同
        // AudioOpusEncoderTests.testEncodeDecodeLoopbackSNR）。
        let frameCount = 6
        var pcmIn: [Int16] = []
        var pcmOut: [Int16] = []
        for f in 0..<frameCount {
            let pcm = (0..<640).map {
                Int16(12000.0 * sin(Double(f * 640 + $0) * 2 * .pi / 160.0))
            }
            guard let packet = encoder.encode(pcm),
                  let decoded = decoder.decode(packet) else {
                check(false, "LAsr: 第 \(f) 帧编解码失败")
                return
            }
            pcmIn.append(contentsOf: pcm)
            pcmOut.append(contentsOf: decoded)
        }
        check(pcmOut.count == 640 * frameCount, "LAsr: 各帧解码 640 采样")
        var bestSnr = -Double.infinity
        let n = min(pcmIn.count, pcmOut.count)
        for lag in 0...960 {
            var signal = 0.0, noise = 0.0
            var i = 640   // 跳过首帧起始瞬态
            while i + lag < n {
                let x = Double(pcmIn[i]), y = Double(pcmOut[i + lag])
                signal += x * x
                noise += (x - y) * (x - y)
                i += 1
            }
            if noise > 0 { bestSnr = max(bestSnr, 10 * log10(signal / noise)) }
        }
        check(bestSnr > 10, "LAsr: AudioOpusDecoder 回环 SNR>10dB（实际 \(String(format: "%.1f", bestSnr))）")
        check(decoder.decode(Data()) == nil, "LAsr: 空 packet 解码返回 nil")
    } catch {
        check(false, "LAsr: 编解码器构造失败 \(error)")
    }

    // ============================================================
    // ③ LocalAsrClient 状态机（假引擎）
    // ============================================================
    final class FakeEngine: SenseVoiceEngine, @unchecked Sendable {
        var calls = 0
        var text = "测试文本"
        let lock = NSLock()
        func decode(_ samples: [Int16]) -> String {
            lock.lock(); defer { lock.unlock() }
            calls += 1
            return text
        }
        func callCount() -> Int {
            lock.lock(); defer { lock.unlock() } 
            return calls
        }
    }

    // start 校验：模型目录未就位（无 engineOverride → 走真实验证路径）。
    do {
        let client = LocalAsrClient(modelsDir: "/nonexistent/vs-test-models")
        let ok = client.start(options: ASRSessionOptions())
        check(!ok, "LAsr: 模型未就位 start 返回 false")
        check(client.lastStartError.contains("未就位"),
              "LAsr: lastStartError 含「未就位」提示")
    }

    // 正常流：partial（首块）→ final（isLast，无新样本复用）。
    do {
        let engine = FakeEngine()
        let client = LocalAsrClient(modelsDir: "/unused", engineOverride: engine)
        var partials: [String] = []
        var finals: [String] = []
        var errors: [String] = []
        client.onPartial = { partials.append($0) }
        client.onFinal = { finals.append($0) }
        client.onError = { errors.append($0) }
        check(client.start(options: ASRSessionOptions()), "LAsr: 假引擎 start 成功")

        let (chunks, _) = try makeOggChunks(frames: 2)
        client.sendOggOpusChunk(chunks[0], isLast: false)
        check(waitUntil { !partials.isEmpty },
              "LAsr: 首块触发 onPartial（首块立即解码）")
        check(engine.callCount() >= 1, "LAsr: 引擎至少推理一次")
        let callsAtPartial = engine.callCount()

        for (i, c) in chunks.dropFirst().enumerated() {
            client.sendOggOpusChunk(c, isLast: i == chunks.count - 2)
        }
        check(waitUntil { !finals.isEmpty }, "LAsr: isLast 触发 onFinal")
        check(engine.callCount() == callsAtPartial + 1,
              "LAsr: final 有新样本再推理一次（calls=\(engine.callCount())）")
        check(finals == ["测试文本"], "LAsr: final 文本 = 引擎输出")
        check(errors.isEmpty, "LAsr: 正常流零错误")
    } catch {
        check(false, "LAsr: 正常流构造失败 \(error)")
    }

    // final 复用：isLast 无新样本 → 不再推理，final = 上次结果。
    do {
        let engine = FakeEngine()
        let client = LocalAsrClient(modelsDir: "/unused", engineOverride: engine)
        var finals: [String] = []
        client.onFinal = { finals.append($0) }
        _ = client.start(options: ASRSessionOptions())
        let (chunks, _) = try makeOggChunks(frames: 1)
        for (i, c) in chunks.enumerated() {
            client.sendOggOpusChunk(c, isLast: i == chunks.count - 1)
        }
        check(waitUntil { !finals.isEmpty }, "LAsr: 单块 isLast 触发 final")
        let callsAfterFirst = engine.callCount()
        client.finish()   // 已 isLast：幂等无操作
        Thread.sleep(forTimeInterval: 0.2)
        check(engine.callCount() == callsAfterFirst,
              "LAsr: finish 幂等不触发多余推理")
    } catch {
        check(false, "LAsr: 复用流构造失败 \(error)")
    }

    // 空文本 partial 抑制（录音起始静音阶段保持 Listening 显示）。
    do {
        let engine = FakeEngine()
        engine.text = ""
        let client = LocalAsrClient(modelsDir: "/unused", engineOverride: engine)
        var partials: [String] = []
        client.onPartial = { partials.append($0) }
        _ = client.start(options: ASRSessionOptions())
        let (chunks, _) = try makeOggChunks(frames: 1)
        for c in chunks {
            client.sendOggOpusChunk(c, isLast: false)
        }
        check(waitUntil { engine.callCount() >= 1 }, "LAsr: 空文本引擎已推理")
        Thread.sleep(forTimeInterval: 0.15)
        check(partials.isEmpty, "LAsr: 空文本 partial 不上报")
        client.cancel()
    } catch {
        check(false, "LAsr: 空文本流构造失败 \(error)")
    }

    // 取消：cancel 后到达的 final 数据不产生任何回调。
    do {
        let engine = FakeEngine()
        let client = LocalAsrClient(modelsDir: "/unused", engineOverride: engine)
        var finals: [String] = []
        var partials: [String] = []
        client.onFinal = { finals.append($0) }
        client.onPartial = { partials.append($0) }
        _ = client.start(options: ASRSessionOptions())
        client.cancel()
        let (chunks, _) = try makeOggChunks(frames: 1)
        for (i, c) in chunks.enumerated() {
            client.sendOggOpusChunk(c, isLast: i == chunks.count - 1)
        }
        Thread.sleep(forTimeInterval: 0.3)
        check(finals.isEmpty && partials.isEmpty, "LAsr: 取消后零回调")
        // 再次 start 复用引擎开启新会话（跨会话引擎复用语义）。
        var finals2: [String] = []
        client.onFinal = { finals2.append($0) }
        check(client.start(options: ASRSessionOptions()), "LAsr: cancel 后可重新 start")
        for (i, c) in chunks.enumerated() {
            client.sendOggOpusChunk(c, isLast: i == chunks.count - 1)
        }
        check(waitUntil { !finals2.isEmpty }, "LAsr: 新会话正常出 final")
    } catch {
        check(false, "LAsr: 取消流构造失败 \(error)")
    }

    // 垃圾音频：partial 阶段静默、final 阶段如实报错。
    do {
        let engine = FakeEngine()
        let client = LocalAsrClient(modelsDir: "/unused", engineOverride: engine)
        var errors: [String] = []
        client.onError = { errors.append($0) }
        _ = client.start(options: ASRSessionOptions())
        client.sendOggOpusChunk(Data("garbage-not-ogg".utf8), isLast: true)
        check(waitUntil { !errors.isEmpty }, "LAsr: 垃圾音频 final 报错")
        check(errors.first?.contains("本地识别失败") == true,
              "LAsr: 错误文案含「本地识别失败」")
    }

    // ============================================================
    // ④ 配置解析与目录解析
    // ============================================================
    do {
        let toml = """
        [local_asr]
        enabled = true
        models_dir = "/opt/vs-models"
        """
        let dir = FileManager.default.temporaryDirectory
            .appendingPathComponent("vs-lasr-\(UUID().uuidString)", isDirectory: true)

        let config = AppConfig.parse(text: toml)
        check(config.localAsr.enabled, "LAsr: [local_asr] enabled 解析")
        check(config.localAsr.modelsDir == "/opt/vs-models", "LAsr: models_dir 解析")
        // 默认（无表）：enabled=false。
        check(!AppConfig.defaults.localAsr.enabled, "LAsr: 默认关闭")

        // resolvedModelsDir 四形态。
        let settings = LocalAsrSettings()
        checkEqual(
            LocalAsrSettings().resolvedModelsDir(configDirectory: dir),
            dir.appendingPathComponent("models/sense-voice-int8-2024-07-17").path,
            "LAsr: 空 models_dir → 默认路径")
        var custom = LocalAsrSettings()
        custom.modelsDir = "~/my-models"
        checkEqual(
            custom.resolvedModelsDir(configDirectory: dir),
            FileManager.default.homeDirectoryForCurrentUser
                .appendingPathComponent("my-models").path,
            "LAsr: ~ 展开")
        custom.modelsDir = "rel-models"
        checkEqual(
            custom.resolvedModelsDir(configDirectory: dir),
            dir.appendingPathComponent("rel-models").path,
            "LAsr: 相对路径锚配置目录")
        custom.modelsDir = "/abs/models"
        checkEqual(custom.resolvedModelsDir(configDirectory: dir), "/abs/models",
                   "LAsr: 绝对路径原样")
        _ = settings

        // 序列化含 [local_asr] 段（恒写出便于发现）。
        let saved = config.serializedText()
        check(saved.contains("[local_asr]"), "LAsr: 序列化含 [local_asr] 段")
        check(saved.contains("models_dir = \"/opt/vs-models\""),
              "LAsr: 序列化保留自定义 models_dir")
    }

    // ============================================================
    // ⑤ validateModelsDir
    // ============================================================
    do {
        let dir = FileManager.default.temporaryDirectory
            .appendingPathComponent("vs-validate-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        check(LocalAsrClient.validateModelsDir(dir.path) != nil,
              "LAsr: 空目录校验失败")
        try Data([0x01]).write(to: dir.appendingPathComponent("model.int8.onnx"))
        try Data("x".utf8).write(to: dir.appendingPathComponent("tokens.txt"))
        check(LocalAsrClient.validateModelsDir(dir.path) == nil,
              "LAsr: 两文件齐备校验通过")
    } catch {
        check(false, "LAsr: 校验测试 IO 失败 \(error)")
    }

    // ============================================================
    // ⑥ 真实模型冒烟（SKIP 纪律：模型/dylib/语料任一缺失即跳过，不 mock）
    //    验证 dlopen 绑定 + SenseVoice 推理 + 「今天天气不错」逐字正确
    //   （评测基线 short_01 CER=0）。
    // ============================================================
    do {
        let repoRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent()
        let corpusPCM = repoRoot
            .appendingPathComponent("scripts/e2e_test/corpus/short_01.pcm")
        let modelsDir = AppConfig.configDirectory
            .appendingPathComponent("models/sense-voice-int8-2024-07-17").path
        let dylibReady = SherpaSenseVoiceEngine.librarySearchPaths()
            .contains { FileManager.default.fileExists(atPath: $0) }
        guard dylibReady,
              LocalAsrClient.validateModelsDir(modelsDir) == nil,
              let pcmBytes = try? Data(contentsOf: corpusPCM), !pcmBytes.isEmpty else {
            print("SKIP LAsr: 真实模型冒烟（模型/dylib/语料未就位；scripts/setup-mac-local-asr.sh 可安装）")
            return
        }
        let engine = try SherpaSenseVoiceEngine(modelsDir: modelsDir)
        let samples = pcmBytes.withUnsafeBytes { raw in
            raw.bindMemory(to: Int16.self).map { $0 }
        }
        let text = engine.decode(samples)
        let expected = "今天天气不错"
        check(text.contains(expected),
              "LAsr: 真实 SenseVoice 冒烟「\(expected)」（实际「\(text)」）")
    } catch {
        check(false, "LAsr: 真实模型冒烟异常 \(error)")
    }
}
