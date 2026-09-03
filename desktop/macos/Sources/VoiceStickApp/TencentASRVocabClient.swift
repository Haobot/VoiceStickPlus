import CryptoKit
import Foundation

/// 腾讯云 ASR 热词表管理 REST API 客户端（逐行移植 Windows tencent_asr_vocab_client）。
///
/// 通过 CreateAsrVocab / UpdateAsrVocab / GetAsrVocabList 接口自动管理名为
/// "VoiceStick-Hotwords" 的热词表，返回 VocabId 供 WebSocket ASR 引用。
/// 签名采用 TC3-HMAC-SHA256（腾讯云 API 3.0 标准）。
///
/// 所有公共方法同步阻塞（内部 semaphore 等待 URLSession），调用方必须在后台线程调用。
final class TencentASRVocabClient {
    private struct HotwordEntry {
        let word: String
        let weight: Int
    }

    private let config: AppConfig

    private static let host = "asr.tencentcloudapi.com"
    private static let service = "asr"
    private static let version = "2019-06-14"
    private static let defaultVocabName = "VoiceStick-Hotwords"
    private static let requestTimeout: TimeInterval = 10

    init(config: AppConfig) {
        self.config = config
    }

    /// 同步热词表：用给定的热词列表创建或更新默认热词表，返回 VocabId；失败返回空串。
    func syncHotwords(_ hotwords: [String]) -> String {
        guard !hotwords.isEmpty else { return "" }
        guard !config.tencentSecretID.isEmpty, !config.tencentSecretKey.isEmpty else { return "" }

        // 过滤：去首尾空白、词长 <= 30 字节、字符白名单（一个非法词会让整表同步被拒，
        // 见 Windows 端 2026-08-01 热词评测实测 InvalidParameterValue.InvalidWordWeight）。
        let entries = hotwords.compactMap { raw -> HotwordEntry? in
            let trimmed = raw.trimmingCharacters(in: .whitespacesAndNewlines)
            guard !trimmed.isEmpty, trimmed.utf8.count <= 30,
                  Self.isValidHotwordChars(trimmed) else { return nil }
            return HotwordEntry(word: trimmed, weight: 10)
        }
        guard !entries.isEmpty else { return "" }

        // 1. 查找已有热词表；2. 存在则更新，否则新建
        var vocabID = findVocabID(name: Self.defaultVocabName)
        let response: String
        if vocabID.isEmpty {
            response = createVocab(name: Self.defaultVocabName, words: entries,
                                   description: "Voice Stick 自动管理热词表")
        } else {
            response = updateVocab(vocabID: vocabID, words: entries)
        }

        // CreateAsrVocab 返回 Response.VocabId；UpdateAsrVocab 不返回，沿用请求中的 id。
        if let responseVocabID = Self.extractResponseVocabID(from: response), !responseVocabID.isEmpty {
            vocabID = responseVocabID
        }
        return vocabID
    }

    /// 腾讯热词词表 API 只接受中英文/数字/连字符/下划线：ASCII 字母数字与 '_' '-' 放行，
    /// 所有 >= 0x80 的 UTF-8 多字节序列（CJK 等）放行，其余（'.'、空格、ASCII 标点）拒绝。
    static func isValidHotwordChars(_ word: String) -> Bool {
        guard !word.isEmpty else { return false }
        for byte in word.utf8 {
            if byte >= 0x80 { continue }
            let isAllowed = (byte >= UInt8(ascii: "0") && byte <= UInt8(ascii: "9"))
                || (byte >= UInt8(ascii: "a") && byte <= UInt8(ascii: "z"))
                || (byte >= UInt8(ascii: "A") && byte <= UInt8(ascii: "Z"))
                || byte == UInt8(ascii: "_") || byte == UInt8(ascii: "-")
            if !isAllowed { return false }
        }
        return true
    }

    // ============================================================
    // TC3-HMAC-SHA256 签名
    // ============================================================

    static func hmacSHA256(key: Data, message: Data) -> Data {
        Data(HMAC<SHA256>.authenticationCode(for: message, using: SymmetricKey(data: key)))
    }

    static func sha256Hex(_ data: Data) -> String {
        SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
    }

    /// 返回完整 Authorization 头值。
    static func tc3Authorization(secretID: String, secretKey: String, action: String,
                                 payload: String, timestamp: Int64) -> String {
        let date = utcDateString(from: timestamp)
        let credentialScope = "\(date)/\(service)/tc3_request"

        let canonicalHeaders =
            "content-type:application/json; charset=utf-8\n" +
            "host:\(host)\n" +
            "x-tc-action:\(action.lowercased())\n"
        let signedHeaders = "content-type;host;x-tc-action"
        let hashedPayload = sha256Hex(Data(payload.utf8))
        let canonicalRequest =
            "POST\n/\n\n" +
            canonicalHeaders + "\n" +
            signedHeaders + "\n" +
            hashedPayload

        let stringToSign =
            "TC3-HMAC-SHA256\n" +
            "\(timestamp)\n" +
            credentialScope + "\n" +
            sha256Hex(Data(canonicalRequest.utf8))

        let secretDate = hmacSHA256(key: Data("TC3\(secretKey)".utf8), message: Data(date.utf8))
        let secretService = hmacSHA256(key: secretDate, message: Data(service.utf8))
        let secretSigning = hmacSHA256(key: secretService, message: Data("tc3_request".utf8))
        let signature = hmacSHA256(key: secretSigning, message: Data(stringToSign.utf8))
            .map { String(format: "%02x", $0) }.joined()

        return "TC3-HMAC-SHA256 Credential=\(secretID)/\(credentialScope), " +
            "SignedHeaders=\(signedHeaders), Signature=\(signature)"
    }

    private static func utcDateString(from timestamp: Int64) -> String {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.timeZone = TimeZone(identifier: "UTC")
        formatter.dateFormat = "yyyy-MM-dd"
        return formatter.string(from: Date(timeIntervalSince1970: TimeInterval(timestamp)))
    }

    // ============================================================
    // REST API 调用（同步）
    // ============================================================

    private func callAPI(_ action: String, payload: String) -> String {
        let timestamp = Int64(Date().timeIntervalSince1970)
        let authorization = Self.tc3Authorization(
            secretID: config.tencentSecretID,
            secretKey: config.tencentSecretKey,
            action: action,
            payload: payload,
            timestamp: timestamp
        )

        var request = URLRequest(url: URL(string: "https://\(Self.host)/")!)
        request.httpMethod = "POST"
        request.timeoutInterval = Self.requestTimeout
        request.setValue(authorization, forHTTPHeaderField: "Authorization")
        request.setValue("application/json; charset=utf-8", forHTTPHeaderField: "Content-Type")
        request.setValue(action.lowercased(), forHTTPHeaderField: "X-TC-Action")
        request.setValue("\(timestamp)", forHTTPHeaderField: "X-TC-Timestamp")
        request.setValue(Self.version, forHTTPHeaderField: "X-TC-Version")
        request.setValue("ap-guangzhou", forHTTPHeaderField: "X-TC-Region")
        request.httpBody = Data(payload.utf8)

        let semaphore = DispatchSemaphore(value: 0)
        var responseBody = ""
        URLSession.shared.dataTask(with: request) { data, _, error in
            defer { semaphore.signal() }
            if let error {
                NSLog("Tencent vocab API \(action) failed: \(error.localizedDescription)")
                return
            }
            if let data {
                responseBody = String(data: data, encoding: .utf8) ?? ""
            }
        }.resume()
        semaphore.wait()
        return responseBody
    }

    private func createVocab(name: String, words: [HotwordEntry], description: String) -> String {
        var payload: [String: Any] = ["Name": name]
        if !description.isEmpty {
            payload["Description"] = description
        }
        if !words.isEmpty {
            payload["WordWeights"] = words.map { ["Word": $0.word, "Weight": $0.weight] }
        }
        return callAPI("CreateAsrVocab", payload: jsonString(payload))
    }

    private func updateVocab(vocabID: String, words: [HotwordEntry]) -> String {
        var payload: [String: Any] = ["VocabId": vocabID]
        if !words.isEmpty {
            payload["WordWeights"] = words.map { ["Word": $0.word, "Weight": $0.weight] }
        }
        return callAPI("UpdateAsrVocab", payload: jsonString(payload))
    }

    private func findVocabID(name: String) -> String {
        let response = callAPI("GetAsrVocabList", payload: #"{"Limit":30}"#)
        guard let data = response.data(using: .utf8),
              let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let responseObject = root["Response"] as? [String: Any],
              let list = responseObject["VocabList"] as? [[String: Any]] else {
            return ""
        }
        for item in list where (item["Name"] as? String) == name {
            if let vocabID = item["VocabId"] as? String {
                return vocabID
            }
        }
        return ""
    }

    private static func extractResponseVocabID(from response: String) -> String? {
        guard let data = response.data(using: .utf8),
              let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let responseObject = root["Response"] as? [String: Any] else {
            return nil
        }
        return responseObject["VocabId"] as? String
    }

    private func jsonString(_ object: [String: Any]) -> String {
        guard let data = try? JSONSerialization.data(withJSONObject: object),
              let text = String(data: data, encoding: .utf8) else {
            return "{}"
        }
        return text
    }
}
