import Foundation

/// OpenAI 兼容 chat/completions SSE 流式响应行解析（对齐 Windows
/// LLMChatClient::ParseSseLine，纯逻辑可单测）。
public enum LlmSseEvent: Equatable {
    /// `data: {"choices":[{"delta":{"content":"…"}}]}` 提取到的增量 token（可为空串）。
    case token(String)
    /// `data: [DONE]`。
    case done
    /// 非 data 行 / 注释行 / 无 delta.content 的 JSON。
    case none
}

public enum LlmSseParser {
    /// 解析单行 SSE 输出。仅处理 "data:" 前缀（容忍前导空格差异）；
    /// "[DONE]" 结束；JSON 解析失败或无 delta.content 返回 .none（对齐 Windows）。
    public static func parseLine(_ line: String) -> LlmSseEvent {
        guard line.hasPrefix("data:") else { return .none }
        var data = String(line.dropFirst(5))
        while data.first == " " { data.removeFirst() }

        if data == "[DONE]" { return .done }

        guard let jsonData = data.data(using: .utf8),
              let object = try? JSONSerialization.jsonObject(with: jsonData) as? [String: Any],
              let choices = object["choices"] as? [[String: Any]],
              let first = choices.first,
              let delta = first["delta"] as? [String: Any],
              let content = delta["content"] as? String else {
            return .none
        }
        return .token(content)
    }
}
