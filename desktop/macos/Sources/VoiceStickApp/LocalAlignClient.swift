import Foundation
import VoiceStickCore

/// targeted 目标词本地对齐客户端（App 合规层；纯逻辑在 VoiceStickCore/VoiceTargetedAlign）。
///
/// 设计（Doc/Plan/voice-edit-commands.md targeted 章·本地对齐）：
/// - 端点：本机 ollama `/api/chat`（`think:false` 关思考、`format` 为
///   reason+answer 结构化 schema、answer 枚举硬约束 = 候选 + NONE）；
/// - **仅允许回环地址**：对齐请求携带最近注入文本，非 `127.0.0.1/::1/localhost`
///   一律拒绝（隐私红线：本地小模型不外发任何内容）；
/// - 同步阻塞接口，由协调器在后台队列调用（attemptLocalAlignOrFallback）；
///   任何失败（网络/超时/解析）返回 nil → 调用方回退普通文本路径。
final class LocalAlignClient {
    private let url: String
    private let model: String
    private let timeout: TimeInterval

    init(url: String, model: String, timeoutMS: Int) {
        self.url = url
        self.model = model
        self.timeout = Double(timeoutMS) / 1000.0
    }

    /// 对齐查询：返回候选之一或 "NONE"；失败返回 nil。线程安全（无共享可变状态）。
    func align(command: String, asrTarget: String,
               remainingText: String, candidates: [String]) -> String? {
        guard let request = makeRequest(
            command: command, asrTarget: asrTarget,
            remainingText: remainingText, candidates: candidates) else { return nil }
        let semaphore = DispatchSemaphore(value: 0)
        var payload: [String: Any]?
        let task = URLSession.shared.dataTask(with: request) { data, _, _ in
            if let data,
               let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
                payload = object
            }
            semaphore.signal()
        }
        task.resume()
        guard semaphore.wait(timeout: .now() + timeout) == .success else {
            task.cancel()
            return nil
        }
        guard let content = (payload?["message"] as? [String: Any])?["content"] as? String else {
            return nil
        }
        return VoiceTargetedAlign.parseAnswer(content, candidates: candidates)
    }

    // MARK: - 请求构造（回环校验）

    private func makeRequest(command: String, asrTarget: String,
                             remainingText: String, candidates: [String]) -> URLRequest? {
        // 隐私红线：对齐请求带最近注入文本，只允许发往本机回环。
        guard let endpoint = URL(string: url.trimmingCharacters(in: .whitespacesAndNewlines)),
              let scheme = endpoint.scheme?.lowercased(),
              scheme == "http" || scheme == "https",
              let host = endpoint.host?.lowercased(),
              host == "127.0.0.1" || host == "localhost" || host == "::1" || host == "[::1]" else {
            NSLog("voice_targeted align endpoint rejected (non-loopback)")
            return nil
        }
        var request = URLRequest(url: endpoint.appendingPathComponent("api/chat"))
        request.timeoutInterval = timeout + 0.5
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        let body = VoiceTargetedAlign.chatRequestBody(
            model: model, command: command, asrTarget: asrTarget,
            remainingText: remainingText, candidates: candidates)
        guard let data = try? JSONSerialization.data(withJSONObject: body) else { return nil }
        request.httpBody = data
        return request
    }
}
