import CryptoKit
import Foundation

/// 腾讯云实时语音识别（WebSocket）客户端（逐行移植 Windows asr_client_tencent.cc）。
///
/// 协议要点（https://cloud.tencent.com/document/product/1093/48982）：
/// - 鉴权：URL 查询参数 HMAC-SHA1 签名（签名原文不含 wss:// 协议头）
/// - 音频数据：二进制帧（Opus, voice_format=10），每帧封装为
///   "opus"(4B) + 小端长度(2B) + Opus 一帧压缩数据
///   （Windows 头注释误写"大端"，实现与单测断言均为小端，此处对齐实现）
/// - 结束识别：文本帧 JSON（{"type":"end"}）
/// - 识别结果：文本帧 JSON（slice_type 0=开始/1=中间/2=单句稳态；顶层 final=1 整段结束）
///
/// 状态机：idle → starting（缓冲音频）→ streaming → finishing → idle
final class TencentASRClient: ASRClient {
    private enum SessionState {
        case idle
        case starting   // WebSocket 连接中，缓冲音频
        case streaming  // 可发送音频数据
        case finishing  // 已发送 end，等待最终结果
    }

    private struct QueuedAudioChunk {
        let data: Data
        let isLast: Bool
    }

    private let config: AppConfig
    private let queue = DispatchQueue(label: "VoiceStick.TencentASRClient")
    private var webSocket: URLSessionWebSocketTask?
    private var sessionState: SessionState = .idle
    private var cancelled = false
    private var finalEmitted = false       // 本会话是否已触发 onFinal（防 final=1 与 close 兜底重复）
    private var currentVoiceID = ""
    private var latestTranscript = ""
    private var accumulatedFinalText = ""  // 累积各句 slice_type=2 稳态文本，整段结束(onFinal)时输出
    private var queuedAudioChunks: [QueuedAudioChunk] = []
    private var emittedDefiniteSegmentKeys: Set<String> = []
    private var sessionOptions = ASRSessionOptions()
    private var cachedVocabID = ""         // 本次运行自动创建的热词表 ID（跨会话复用）

    var onPartial: ((String) -> Void)?
    var onSegment: ((ASRSegment) -> Void)?
    var onFinal: ((String) -> Void)?
    var onError: ((String) -> Void)?
    var onUpgradeURL: ((URL, String) -> Void)?

    init(config: AppConfig) {
        self.config = config
    }

    deinit {
        webSocket?.cancel(with: .goingAway, reason: nil)
    }

    // ============================================================
    // ASRClient 接口
    // ============================================================

    @discardableResult
    func start(options: ASRSessionOptions) -> Bool {
        guard !config.tencentSecretID.isEmpty, !config.tencentSecretKey.isEmpty else {
            NSLog("TencentAsr config error: missing SecretId/SecretKey")
            notifyError("缺少腾讯云 SecretId/SecretKey")
            return false
        }
        guard !config.tencentAppid.isEmpty else {
            NSLog("TencentAsr config error: missing AppId")
            notifyError("缺少腾讯云 AppId")
            return false
        }
        var options = options
        if options.hotwords.isEmpty {
            options.hotwords = config.asrHotwords
        }
        sessionOptions = options
        queue.async { [weak self] in
            self?.beginSession()
        }
        return true
    }

    func sendOggOpusChunk(_ data: Data, isLast: Bool) {
        queue.async { [weak self] in
            self?.sendAudio(data, isLast: isLast)
        }
    }

    func finish() {
        queue.async { [weak self] in
            self?.finishSessionIfNeeded()
        }
    }

    func cancel() {
        queue.async { [weak self] in
            self?.cancelSession()
        }
    }

    // ============================================================
    // 会话生命周期（全部在 queue 上执行）
    // ============================================================

    private func beginSession() {
        guard sessionState == .idle else {
            notifyError("ASR session already active")
            return
        }

        currentVoiceID = Self.generateVoiceID()
        queuedAudioChunks.removeAll(keepingCapacity: true)
        latestTranscript = ""
        accumulatedFinalText = ""
        emittedDefiniteSegmentKeys.removeAll(keepingCapacity: true)
        finalEmitted = false
        cancelled = false
        sessionState = .starting

        // 热词自动同步：配置未指定 hotword_id 且配置了热词时，创建/更新热词表并引用。
        // 同步 HTTP 在本后台队列上阻塞执行（对齐 Windows 在连接前同步完成的语义），
        // 期间到达的音频块按 starting 状态缓冲，连接成功后统一补发。
        if config.tencentHotwordID.isEmpty && cachedVocabID.isEmpty && !sessionOptions.hotwords.isEmpty {
            let syncedID = TencentASRVocabClient(config: config).syncHotwords(sessionOptions.hotwords)
            if !syncedID.isEmpty {
                cachedVocabID = syncedID
            }
        }

        connectWebSocket()
    }

    private func connectWebSocket() {
        // 热词表 ID：优先配置值，其次自动同步缓存
        var hotwordID = config.tencentHotwordID
        if hotwordID.isEmpty {
            hotwordID = cachedVocabID
        }
        let url = Self.buildSignedURL(config: config, voiceID: currentVoiceID, hotwordID: hotwordID)
        guard let wsURL = URL(string: url) else {
            failSession("无效的腾讯云 ASR URL")
            return
        }

        NSLog("TencentAsr connecting voice_id=\(currentVoiceID)")
        let task = URLSession.shared.webSocketTask(with: wsURL)
        webSocket = task
        task.resume()

        // 腾讯云实时 ASR 官方文档：握手成功后客户端直接上传音频数据，不等待服务端确认帧。
        // URLSession 会把握手完成前的 send 排队到连接建立后发出，因此直接转 streaming 补发缓冲。
        if sessionState == .starting {
            sessionState = .streaming
        }
        flushQueuedAudioChunks()
        receiveLoop()
    }

    private func cancelSession() {
        if sessionState == .starting || sessionState == .streaming || sessionState == .finishing {
            sendTextFrame(Self.makeEndMessage())
        }
        cancelled = true
        resetSession()
        webSocket?.cancel(with: .goingAway, reason: nil)
        webSocket = nil
    }

    private func failSession(_ message: String) {
        let wasCancelled = cancelled
        let hadActiveSession = sessionState != .idle
        cancelled = true
        resetSession()
        webSocket?.cancel(with: .goingAway, reason: nil)
        webSocket = nil
        if !wasCancelled && hadActiveSession {
            notifyError(message)
        }
    }

    private func resetSession() {
        queuedAudioChunks.removeAll(keepingCapacity: true)
        currentVoiceID = ""
        latestTranscript = ""
        accumulatedFinalText = ""
        emittedDefiniteSegmentKeys.removeAll(keepingCapacity: true)
        sessionState = .idle
    }

    // ============================================================
    // 音频发送（queue 上）
    // ============================================================

    private func sendAudio(_ data: Data, isLast: Bool) {
        switch sessionState {
        case .starting:
            queuedAudioChunks.append(QueuedAudioChunk(data: data, isLast: isLast))
            return
        case .idle, .finishing:
            return
        case .streaming:
            break
        }

        let frame = Self.extractTencentOpusFrame(data)
        if !frame.isEmpty {
            sendBinaryFrame(frame)
        }
        if isLast {
            finishSessionIfNeeded()
        }
    }

    private func flushQueuedAudioChunks() {
        let chunks = queuedAudioChunks
        queuedAudioChunks.removeAll(keepingCapacity: true)
        for chunk in chunks {
            let frame = Self.extractTencentOpusFrame(chunk.data)
            if !frame.isEmpty {
                sendBinaryFrame(frame)
            }
            if chunk.isLast {
                finishSessionIfNeeded()
            }
        }
    }

    private func finishSessionIfNeeded() {
        if sessionState == .starting {
            // 音频已到达但连接尚未建立，将 end 标记加入缓冲
            if !queuedAudioChunks.contains(where: \.isLast) {
                queuedAudioChunks.append(QueuedAudioChunk(data: Data(), isLast: true))
            }
            return
        }
        guard sessionState == .streaming, !currentVoiceID.isEmpty else { return }
        sessionState = .finishing
        NSLog("TencentAsr sending end message voice_id=\(currentVoiceID)")
        sendTextFrame(Self.makeEndMessage())
    }

    // ============================================================
    // WebSocket 收发（queue 上）
    // ============================================================

    private func sendTextFrame(_ text: String) {
        send(.string(text))
    }

    private func sendBinaryFrame(_ data: Data) {
        send(.data(data))
    }

    private func send(_ message: URLSessionWebSocketTask.Message) {
        guard let task = webSocket else { return }
        task.send(message) { [weak self] error in
            guard let self, let error else { return }
            self.queue.async {
                guard self.webSocket === task else { return }
                self.failSession("腾讯云 ASR 发送失败: \(error.localizedDescription)")
            }
        }
    }

    private func receiveLoop() {
        let receivingTask = webSocket
        receivingTask?.receive { [weak self] result in
            guard let self else { return }
            self.queue.async {
                guard self.webSocket === receivingTask else { return }
                switch result {
                case .success(let message):
                    if case .string(let text) = message {
                        self.handleTextResponse(text)
                    }
                    // 忽略二进制帧（腾讯云不发送二进制帧）
                    if self.webSocket === receivingTask {
                        self.receiveLoop()
                    }
                case .failure(let error):
                    self.handleConnectionClosed(error)
                }
            }
        }
    }

    /// 连接关闭/接收失败兜底：正常流程服务端先发 final=1 再断开（emitFinalText 已触发
    /// onFinal）。若服务端异常只断开未发 final=1，且本会话已发 end（finishing）并有累积
    /// 文本，则补触发一次 onFinal，避免 button_up 后丢文本。主动取消（cancelled）不补。
    private func handleConnectionClosed(_ error: Error) {
        webSocket = nil
        if cancelled || sessionState == .idle {
            return
        }
        if !finalEmitted && sessionState == .finishing && !accumulatedFinalText.isEmpty {
            NSLog("TencentAsr connection closed without final=1, emitting accumulated text as fallback")
            emitFinalText()
            return
        }
        failSession("腾讯云 ASR WebSocket 接收失败: \(error.localizedDescription)")
    }

    // ============================================================
    // 结果解析（queue 上）
    // ============================================================

    private func handleTextResponse(_ text: String) {
        guard let data = text.data(using: .utf8),
              let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            return
        }

        let code = root["code"] as? Int ?? -1
        if code != 0 {
            var message = root["message"] as? String ?? ""
            if message.isEmpty {
                message = "腾讯云 ASR 错误 (code=\(code))"
            }
            // 某些错误码可以恢复（如资源包耗尽），通过 onUpgradeURL 通知
            if code == 4004 || code == 4005, let url = URL(string: "https://console.cloud.tencent.com/asr") {
                let upgradeMessage = message
                DispatchQueue.main.async { [weak self] in
                    self?.onUpgradeURL?(url, upgradeMessage)
                }
            }
            failSession(message)
            return
        }

        // 整段识别结束信号：顶层 final=1（无 result 字段），服务端随后断开连接。
        if (root["final"] as? Int ?? 0) == 1 {
            emitFinalText()
            return
        }

        if let result = root["result"] as? [String: Any] {
            let sliceType = result["slice_type"] as? Int ?? -1
            let voiceText = result["voice_text_str"] as? String

            if sliceType == 0 || sliceType == 1 {
                // 中间结果（非稳态）
                if let voiceText, !voiceText.isEmpty {
                    latestTranscript = voiceText
                    DispatchQueue.main.async { [weak self] in
                        self?.onPartial?(voiceText)
                    }
                }
                emitWordListSegments(from: root)
            } else if sliceType == 2 {
                // 单句稳态结果（VAD 切句）：仅累积该句文本，不触发 onFinal、不重置会话，
                // 保持 streaming 以便继续接收后续句子与 button_up 的 end。
                let sentence = (voiceText?.isEmpty == false) ? voiceText! : latestTranscript
                accumulatedFinalText = Self.accumulateSentence(current: accumulatedFinalText, sentence: sentence)
                emitWordListSegments(from: root)
                latestTranscript = ""
            }
            return
        }

        // 握手确认或控制消息确认（无 result 字段）
        if sessionState == .starting {
            sessionState = .streaming
            flushQueuedAudioChunks()
        }
    }

    private func emitWordListSegments(from root: [String: Any]) {
        let segments = Self.extractWordListSegments(from: root, emittedKeys: &emittedDefiniteSegmentKeys)
        guard !segments.isEmpty else { return }
        DispatchQueue.main.async { [weak self] in
            for segment in segments {
                self?.onSegment?(segment)
            }
        }
    }

    /// 整段识别结束（收到 final=1 或连接关闭兜底）时输出累积全文并重置会话。
    /// 受 finalEmitted 守卫保护，本会话只触发一次 onFinal；cancelled 时不触发。
    private func emitFinalText() {
        guard !cancelled, !finalEmitted else { return }
        finalEmitted = true
        var text = accumulatedFinalText
        if text.isEmpty {
            text = latestTranscript  // 兜底：仅有中间结果未稳态
        }
        resetSession()
        DispatchQueue.main.async { [weak self] in
            self?.onFinal?(text)
        }
    }

    private func notifyError(_ message: String) {
        DispatchQueue.main.async { [weak self] in
            self?.onError?(message)
        }
    }

    // ============================================================
    // 静态工具方法（internal 以便日后单测）
    // ============================================================

    /// URL 查询参数签名：HMAC-SHA1 → Base64 → URL Encode。
    /// 签名原文：asr.cloud.tencent.com/asr/v2/<appid>?<按 key 字典序排列的查询参数>（不含协议头）。
    static func buildSignedURL(config: AppConfig, voiceID: String, hotwordID: String? = nil) -> String {
        let timestamp = Int64(Date().timeIntervalSince1970)
        let expired = timestamp + 86400  // 24 小时后过期
        let nonce = UInt32.random(in: UInt32.min ... UInt32.max)

        var params: [(key: String, value: String)] = [
            ("secretid", config.tencentSecretID),
            ("timestamp", "\(timestamp)"),
            ("expired", "\(expired)"),
            ("nonce", "\(nonce)"),
            ("engine_model_type", config.tencentEngineModelType),
            ("voice_format", "10"),  // Opus
            ("needvad", "1"),
            ("voice_id", voiceID)
        ]
        let effectiveHotwordID = hotwordID ?? config.tencentHotwordID
        if !effectiveHotwordID.isEmpty {
            params.append(("hotword_id", effectiveHotwordID))
        }

        // 按 key 字典序排序
        params.sort { $0.key < $1.key }
        let query = params.map { "\($0.key)=\($0.value)" }.joined(separator: "&")

        let signString = "asr.cloud.tencent.com/asr/v2/\(config.tencentAppid)?\(query)"
        let hmacResult = hmacSHA1(key: config.tencentSecretKey, message: signString)
        let signature = urlEncode(base64Encode(hmacResult))

        return "wss://asr.cloud.tencent.com/asr/v2/\(config.tencentAppid)?\(query)&signature=\(signature)"
    }

    static func hmacSHA1(key: String, message: String) -> Data {
        Data(HMAC<Insecure.SHA1>.authenticationCode(
            for: Data(message.utf8),
            using: SymmetricKey(data: Data(key.utf8))
        ))
    }

    static func base64Encode(_ data: Data) -> String {
        data.base64EncodedString()
    }

    /// URL 编码：非保留字符（字母数字与 - _ . ~）原样，其余 %XX 大写。
    static func urlEncode(_ value: String) -> String {
        var output = ""
        output.reserveCapacity(value.utf8.count)
        for byte in value.utf8 {
            let isUnreserved = (byte >= UInt8(ascii: "0") && byte <= UInt8(ascii: "9"))
                || (byte >= UInt8(ascii: "a") && byte <= UInt8(ascii: "z"))
                || (byte >= UInt8(ascii: "A") && byte <= UInt8(ascii: "Z"))
                || byte == UInt8(ascii: "-") || byte == UInt8(ascii: "_")
                || byte == UInt8(ascii: ".") || byte == UInt8(ascii: "~")
            if isUnreserved {
                output.append(Character(UnicodeScalar(byte)))
            } else {
                output += String(format: "%%%02X", byte)
            }
        }
        return output
    }

    /// UUID v4 文本形式（小写带连字符，对齐 Windows GenerateVoiceId）。
    static func generateVoiceID() -> String {
        UUID().uuidString.lowercased()
    }

    /// 腾讯云实时 ASR 结束消息固定为 {"type":"end"}（小写，不带 voice_id）。
    static func makeEndMessage() -> String {
        #"{"type":"end"}"#
    }

    /// 将 Ogg Opus 页面中的单个 Opus 包转换为腾讯云实时 ASR 要求的封装格式：
    /// "opus"（4 字节）+ 帧数据长度（2 字节，小端）+ Opus 一帧压缩数据。
    /// 返回空 Data 表示该数据不是可发送的音频帧（如 OpusHead/OpusTags）。
    static func extractTencentOpusFrame(_ data: Data) -> Data {
        guard data.count >= 27,
              data[data.startIndex] == UInt8(ascii: "O"),
              data[data.startIndex + 1] == UInt8(ascii: "g"),
              data[data.startIndex + 2] == UInt8(ascii: "g"),
              data[data.startIndex + 3] == UInt8(ascii: "S") else {
            return Data()
        }
        let pageSegments = Int(data[data.startIndex + 26])
        let headerSize = 27 + pageSegments
        guard headerSize < data.count else { return Data() }
        let raw = data.subdata(in: (data.startIndex + headerSize)..<data.endIndex)
        // OpusHead/OpusTags 头包以 "Opus" 开头（4 字节），但不是音频帧
        if raw.count >= 4,
           raw[raw.startIndex] == UInt8(ascii: "O"),
           raw[raw.startIndex + 1] == UInt8(ascii: "p"),
           raw[raw.startIndex + 2] == UInt8(ascii: "u"),
           raw[raw.startIndex + 3] == UInt8(ascii: "s") {
            return Data()
        }
        // 当前 OggOpusMuxer 每页只放一个 Opus 包（packet <= 255），所以 raw 即为一帧。
        var frame = Data()
        frame.reserveCapacity(4 + 2 + raw.count)
        frame.append(contentsOf: [UInt8(ascii: "o"), UInt8(ascii: "p"), UInt8(ascii: "u"), UInt8(ascii: "s")])
        frame.append(UInt8(raw.count & 0xFF))
        frame.append(UInt8((raw.count >> 8) & 0xFF))
        frame.append(raw)
        return frame
    }

    /// 聚合 result.word_list 中所有稳态词（stable_flag == 1）为一个 definite 段；
    /// 已 emit 过的段（按 start:end:text 判重）不重复返回。
    static func extractWordListSegments(from root: [String: Any],
                                        emittedKeys: inout Set<String>) -> [ASRSegment] {
        guard let result = root["result"] as? [String: Any],
              let wordList = result["word_list"] as? [[String: Any]] else {
            return []
        }

        var accumulatedText = ""
        var segmentStart: Int?
        var segmentEnd: Int?
        for wordObject in wordList {
            guard let word = wordObject["word"] as? String else { continue }
            let isStable = (wordObject["stable_flag"] as? Int ?? 0) == 1
            let start = wordObject["start_time"] as? Int ?? 0
            let end = wordObject["end_time"] as? Int ?? 0
            if isStable && !word.isEmpty {
                accumulatedText += word
                if segmentStart == nil { segmentStart = start }
                segmentEnd = end
            }
        }

        guard !accumulatedText.isEmpty else { return [] }
        let segment = ASRSegment(
            text: accumulatedText,
            definite: true,
            startTime: segmentStart,
            endTime: segmentEnd
        )
        let key = segmentKey(startMs: segment.startTime ?? -1, endMs: segment.endTime ?? -1, text: segment.text)
        guard !emittedKeys.contains(key) else { return [] }
        emittedKeys.insert(key)
        return [segment]
    }

    static func segmentKey(startMs: Int, endMs: Int, text: String) -> String {
        "\(startMs):\(endMs):\(text)"
    }

    /// 把单句稳态结果累积到已确定文本上。空 sentence 不改变累积。
    static func accumulateSentence(current: String, sentence: String) -> String {
        if sentence.isEmpty { return current }
        if current.isEmpty { return sentence }
        return current + sentence
    }
}

/// 按 asr_provider 创建对应 ASR 客户端（对齐 Windows win32_app.cc 的 make_asr 工厂）。
enum ASRClientFactory {
    static func makeClient(config: AppConfig) -> any ASRClient {
        switch config.asrProvider {
        case .tencent:
            return TencentASRClient(config: config)
        case .voiceStickCloud, .volcengine:
            return ASRWebSocketClient(config: config)
        }
    }
}
