import Foundation
import CZlib

final class VolcengineASRClient {
    private let config: AppConfig
    private var webSocket: URLSessionWebSocketTask?
    private var sequence: Int32 = 0
    private var finished = false

    var onPartial: ((String) -> Void)?
    var onFinal: ((String) -> Void)?
    var onError: ((String) -> Void)?
    var onUpgradeURL: ((URL) -> Void)?

    init(config: AppConfig) {
        self.config = config
    }

    @discardableResult
    func start() -> Bool {
        let apiKey = activeAPIKey
        guard !apiKey.isEmpty else {
            let message = "Missing ASR API key"
            NSLog("ASR config error: \(message)")
            onError?(message)
            return false
        }

        guard let url = URL(string: activeWebSocketURL) else {
            onError?("Invalid ASR URL")
            return false
        }

        var request = URLRequest(url: url)
        let connectID = UUID().uuidString
        request.setValue(apiKey, forHTTPHeaderField: "X-Api-Key")
        if config.asrProvider == .volcengine {
            request.setValue(config.resourceID, forHTTPHeaderField: "X-Api-Resource-Id")
        }
        request.setValue(connectID, forHTTPHeaderField: "X-Api-Request-Id")
        request.setValue("-1", forHTTPHeaderField: "X-Api-Sequence")

        sequence = 0
        finished = false
        NSLog("ASR start provider=\(config.asrProvider.rawValue) connect_id=\(connectID)")
        let task = URLSession.shared.webSocketTask(with: request)
        webSocket = task
        task.resume()
        receiveLoop()
        sendFullClientRequest()
        return true
    }

    private var activeAPIKey: String {
        switch config.asrProvider {
        case .voiceStickCloud:
            return config.voiceStickAPIKey.trimmingCharacters(in: .whitespacesAndNewlines)
        case .volcengine:
            return config.volcengineAPIKey.trimmingCharacters(in: .whitespacesAndNewlines)
        }
    }

    private var activeWebSocketURL: String {
        switch config.asrProvider {
        case .voiceStickCloud:
            return config.voiceStickCloudURL.trimmingCharacters(in: .whitespacesAndNewlines)
        case .volcengine:
            return AppConfig.volcengineWebSocketURL
        }
    }

    func sendOggOpusChunk(_ data: Data, isLast: Bool) {
        if isLast {
            finished = true
        }
        sendAudio(data, isLast: isLast)
    }

    func finish() {
        guard !finished else { return }
        finished = true
        sendAudio(Data(), isLast: true)
    }

    func cancel() {
        finished = true
        webSocket?.cancel(with: .goingAway, reason: nil)
        webSocket = nil
    }

    private func sendFullClientRequest() {
        let request: [String: Any] = [
            "model_name": "bigmodel",
            "enable_nonstream": true,
            "show_utterances": false,
            "enable_ddc": true
        ]

        let payload: [String: Any] = [
            "user": ["uid": "voice-stick-local"],
            "audio": ["format": "ogg", "codec": "opus", "rate": 16_000, "bits": 16, "channel": 1],
            "request": request
        ]

        guard let payloadData = try? JSONSerialization.data(withJSONObject: payload) else {
            onError?("Invalid ASR request JSON")
            return
        }
        sendBinaryFrame(
            messageType: 0x01,
            flags: 0x00,
            serialization: 0x01,
            compression: 0x01,
            payload: payloadData
        )
    }

    private func sendAudio(_ data: Data, isLast: Bool) {
        sendBinaryFrame(
            messageType: 0x02,
            flags: isLast ? 0x02 : 0x00,
            serialization: 0x00,
            compression: 0x01,
            payload: data
        )
    }

    private func sendBinaryFrame(
        messageType: UInt8,
        flags: UInt8,
        serialization: UInt8,
        compression: UInt8,
        payload: Data
    ) {
        do {
            let compressed = try payload.gzipCompressed()
            var frame = Data()
            frame.append(0x11)
            frame.append((messageType << 4) | flags)
            frame.append((serialization << 4) | compression)
            frame.append(0x00)
            frame.append(contentsOf: UInt32(compressed.count).bigEndianBytes)
            frame.append(compressed)
            webSocket?.send(.data(frame)) { [weak self] error in
                if let error {
                    NSLog("ASR send error: \(error.localizedDescription)")
                    if self?.finished != true {
                        self?.onError?(error.localizedDescription)
                    }
                }
            }
        } catch {
            NSLog("ASR gzip error: \(error.localizedDescription)")
            onError?(error.localizedDescription)
        }
    }

    private func receiveLoop() {
        webSocket?.receive { [weak self] result in
            guard let self else { return }
            switch result {
            case .success(let message):
                self.handle(message)
                self.receiveLoop()
            case .failure(let error):
                if !self.finished {
                    NSLog("ASR receive error: \(error.localizedDescription)")
                    self.onError?(error.localizedDescription)
                }
            }
        }
    }

    private func handle(_ message: URLSessionWebSocketTask.Message) {
        switch message {
        case .string(let text):
            handleASRText(text, isFinalResponse: false)
        case .data(let data):
            handleBinaryResponse(data)
        @unknown default:
            break
        }
    }

    private func handleBinaryResponse(_ data: Data) {
        guard data.count >= 4 else {
            onError?("Short ASR response")
            return
        }

        let messageType = data[1] >> 4
        let flags = data[1] & 0x0f
        let compression = data[2] & 0x0f
        var offset = Int(data[0] & 0x0f) * 4

        switch messageType {
        case 0x09:
            if flags == 0x01 || flags == 0x03 {
                guard data.count >= offset + 4 else { return }
                sequence = Int32(bigEndianBytes: data[offset..<(offset + 4)])
                offset += 4
            }
            guard data.count >= offset + 4 else { return }
            let payloadSize = Int(UInt32(bigEndianBytes: data[offset..<(offset + 4)]))
            offset += 4
            guard data.count >= offset + payloadSize else { return }
            let payload = data.subdata(in: offset..<(offset + payloadSize))
            do {
                let body = compression == 0x01 ? try payload.gzipDecompressed() : payload
                if let text = String(data: body, encoding: .utf8) {
                    logASRResponse(text, isFinalResponse: flags == 0x03)
                    handleASRText(text, isFinalResponse: flags == 0x03)
                }
            } catch {
                NSLog("ASR decompress error: \(error.localizedDescription)")
                onError?(error.localizedDescription)
            }

        case 0x0f:
            guard data.count >= offset + 8 else {
                onError?("Malformed ASR error response")
                return
            }
            let code = UInt32(bigEndianBytes: data[offset..<(offset + 4)])
            offset += 4
            let messageSize = Int(UInt32(bigEndianBytes: data[offset..<(offset + 4)]))
            offset += 4
            guard data.count >= offset + messageSize else {
                onError?("Malformed ASR error response")
                return
            }
            let message = String(data: data.subdata(in: offset..<(offset + messageSize)), encoding: .utf8) ?? "Unknown ASR error"
            NSLog("ASR server error code=\(code): \(message)")
            let parsedError = parsedErrorMessage(code: code, message: message)
            onError?(parsedError.message)
            if let upgradeURL = parsedError.upgradeURL {
                onUpgradeURL?(upgradeURL)
            }

        default:
            NSLog("ASR unhandled response type=\(messageType) bytes=\(data.count)")
        }
    }

    private func parsedErrorMessage(code: UInt32, message: String) -> (message: String, upgradeURL: URL?) {
        guard
            let data = message.data(using: .utf8),
            let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
        else {
            return ("ASR \(code): \(message)", nil)
        }

        let detail = (object["message"] as? String) ?? (object["error"] as? String) ?? message
        if let upgradeURL = object["upgrade_url"] as? String, !upgradeURL.isEmpty {
            return ("ASR \(code): \(detail)", URL(string: upgradeURL))
        }
        return ("ASR \(code): \(detail)", nil)
    }

    private func handleASRText(_ text: String, isFinalResponse: Bool) {
        let transcript = extractTranscript(from: text)

        if isFinalResponse {
            onFinal?(transcript)
        } else if !transcript.isEmpty {
            onPartial?(transcript)
        }
    }

    private func logASRResponse(_ text: String, isFinalResponse: Bool) {
        guard let object = parseJSONPayload(text) else {
            NSLog("ASR response seq=\(sequence) \(responseKind(isFinalResponse)) bytes=\(text.utf8.count)")
            return
        }

        let result = object["result"] as? [String: Any]
        let audioInfo = object["audio_info"] as? [String: Any]
        let duration = audioInfo?["duration"] as? Int
        let isPrefetch = (object["prefetch"] as? Bool) == true || (result?["prefetch"] as? Bool) == true
        let transcript = extractTranscript(from: object)
        let utteranceCount = (result?["utterances"] as? [[String: Any]])?.count

        var parts = [
            "ASR response seq=\(sequence)",
            responseKind(isFinalResponse)
        ]
        if let duration {
            parts.append("duration=\(duration)ms")
        }
        if isPrefetch {
            parts.append("prefetch")
        }
        parts.append("text_len=\(transcript.count)")
        if let utteranceCount {
            parts.append("utterances=\(utteranceCount)")
        }
        parts.append("preview=\"\(transcript.asrLogPreview)\"")
        NSLog(parts.joined(separator: " "))
    }

    private func extractTranscript(from text: String) -> String {
        guard let object = parseJSONPayload(text) else {
            return text
        }

        return extractTranscript(from: object)
    }

    private func extractTranscript(from object: [String: Any]) -> String {
        if let result = object["result"] as? [String: Any] {
            if let value = result["text"] as? String {
                return value
            }
            if let utterances = result["utterances"] as? [[String: Any]] {
                let parts = utterances.compactMap { $0["text"] as? String }
                if !parts.isEmpty {
                    return parts.joined()
                }
            }
        }

        if let results = object["result"] as? [[String: Any]] {
            let parts = results.compactMap { $0["text"] as? String }
            if !parts.isEmpty {
                return parts.joined()
            }
        }

        return ""
    }

    private func parseJSONPayload(_ text: String) -> [String: Any]? {
        guard let data = text.data(using: .utf8) else { return nil }
        return try? JSONSerialization.jsonObject(with: data) as? [String: Any]
    }

    private func responseKind(_ isFinalResponse: Bool) -> String {
        isFinalResponse ? "final" : "partial"
    }
}

private extension String {
    var asrLogPreview: String {
        let normalized = split(whereSeparator: \.isNewline).joined(separator: " ")
        let limit = 48
        guard normalized.count > limit else { return normalized }
        return String(normalized.suffix(limit))
    }
}

private extension FixedWidthInteger {
    var bigEndianBytes: [UInt8] {
        withUnsafeBytes(of: self.bigEndian) { Array($0) }
    }
}

private extension UInt32 {
    init(bigEndianBytes bytes: Data.SubSequence) {
        self = bytes.reduce(0) { ($0 << 8) | UInt32($1) }
    }
}

private extension Int32 {
    init(bigEndianBytes bytes: Data.SubSequence) {
        self = Int32(bitPattern: UInt32(bigEndianBytes: bytes))
    }
}

private extension Data {
    func gzipCompressed() throws -> Data {
        try withZStream(input: self, operation: Z_DEFLATED, windowBits: MAX_WBITS + 16)
    }

    func gzipDecompressed() throws -> Data {
        try withZStream(input: self, operation: Z_DEFLATED, windowBits: MAX_WBITS + 16, decompress: true)
    }

    private func withZStream(input: Data, operation: Int32, windowBits: Int32, decompress: Bool = false) throws -> Data {
        var stream = z_stream()
        let initResult: Int32
        if decompress {
            initResult = inflateInit2_(&stream, windowBits, ZLIB_VERSION, Int32(MemoryLayout<z_stream>.size))
        } else {
            initResult = deflateInit2_(&stream, Z_DEFAULT_COMPRESSION, operation, windowBits, 8, Z_DEFAULT_STRATEGY, ZLIB_VERSION, Int32(MemoryLayout<z_stream>.size))
        }
        guard initResult == Z_OK else {
            throw ASRCompressionError.initFailed(initResult)
        }
        defer {
            if decompress {
                inflateEnd(&stream)
            } else {
                deflateEnd(&stream)
            }
        }

        let chunkSize = 16 * 1024
        var output = Data()
        var buffer = [UInt8](repeating: 0, count: chunkSize)

        return try input.withUnsafeBytes { rawBuffer in
            stream.next_in = UnsafeMutablePointer<Bytef>(mutating: rawBuffer.bindMemory(to: Bytef.self).baseAddress)
            stream.avail_in = uInt(input.count)

            repeat {
                let result = buffer.withUnsafeMutableBytes { outBuffer -> Int32 in
                    stream.next_out = outBuffer.bindMemory(to: Bytef.self).baseAddress
                    stream.avail_out = uInt(chunkSize)
                    return decompress ? inflate(&stream, Z_NO_FLUSH) : deflate(&stream, Z_FINISH)
                }

                guard result == Z_OK || result == Z_STREAM_END else {
                    throw ASRCompressionError.streamFailed(result)
                }

                output.append(buffer, count: chunkSize - Int(stream.avail_out))

                if result == Z_STREAM_END {
                    break
                }
            } while stream.avail_out == 0 || stream.avail_in > 0

            return output
        }
    }
}

private enum ASRCompressionError: LocalizedError {
    case initFailed(Int32)
    case streamFailed(Int32)

    var errorDescription: String? {
        switch self {
        case .initFailed(let code):
            return "zlib init failed: \(code)"
        case .streamFailed(let code):
            return "zlib stream failed: \(code)"
        }
    }
}
