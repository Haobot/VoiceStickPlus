import Foundation

/// ASR 文本精修客户端（对齐 Windows llm_refinement_client.cc）：OpenAI 兼容
/// chat/completions，非流式一次取回；best-effort——任何失败/热词丢失都回退原文。
final class LLMRefinementClient {
    private let config: AppConfig
    private let session: URLSession

    init(config: AppConfig, session: URLSession = .shared) {
        self.config = config
        self.session = session
    }

    /// 精修文本。completion 参数为精修结果；失败或热词被吞时回传 nil（调用侧回退原文）。
    func refine(_ text: String, hotwords: [String], completion: @escaping (String?) -> Void) {
        let apiKey = config.llmAPIKey.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !apiKey.isEmpty, let url = chatCompletionsURL() else {
            completion(nil)
            return
        }

        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.timeoutInterval = 8
        request.setValue("Bearer \(apiKey)", forHTTPHeaderField: "Authorization")
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")

        var payload: [String: Any] = [
            "model": config.llmModel,
            "temperature": 0,
            "messages": [
                ["role": "system", "content": Self.buildPrompt(override: config.refinePrompt, hotwords: hotwords)],
                ["role": "user", "content": text]
            ]
        ]
        // 关闭推理型模型深度思考（对齐 Windows：两种风格都发，DashScope/Qwen 兼容 +
        // vLLM/SGLang chat_template_kwargs）。
        if config.llmDisableThinking {
            payload["enable_thinking"] = false
            payload["chat_template_kwargs"] = ["enable_thinking": false]
        }

        do {
            request.httpBody = try JSONSerialization.data(withJSONObject: payload)
        } catch {
            completion(nil)
            return
        }

        session.dataTask(with: request) { data, _, error in
            guard error == nil,
                  let data,
                  let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                  let choices = object["choices"] as? [[String: Any]],
                  let message = choices.first?["message"] as? [String: Any],
                  let content = message["content"] as? String else {
                completion(nil)
                return
            }
            let refined = content.trimmingCharacters(in: .whitespacesAndNewlines)
            // 热词保护：原文中已正确出现的热词在精修结果里必须原样保留
            //（对齐 Windows RefineResultKeepsHotwords），否则视为精修失败回退原文。
            guard !refined.isEmpty,
                  Self.resultKeepsHotwords(original: text, refined: refined, hotwords: hotwords) else {
                completion(nil)
                return
            }
            completion(refined)
        }.resume()
    }

    private func chatCompletionsURL() -> URL? {
        let base = config.llmBaseURL.trimmingCharacters(in: CharacterSet(charactersIn: "/ \n\r\t"))
        guard !base.isEmpty else { return nil }
        if base.hasSuffix("/chat/completions") {
            return URL(string: base)
        }
        return URL(string: "\(base)/chat/completions")
    }

    /// 默认精修 prompt 逐字对齐 Windows BuildRefinePrompt（含热词表后缀与示例）。
    static func buildPrompt(override promptOverride: String, hotwords: [String]) -> String {
        let trimmed = promptOverride.trimmingCharacters(in: .whitespacesAndNewlines)
        var prompt: String
        if !trimmed.isEmpty {
            prompt = trimmed
        } else {
            prompt = """
            你是一个语音识别后处理器。
            输入为自动语音识别生成的原始文本。请将其改写为规范的书面文本：

            • 去除因语音停顿产生的多余空格，尤其中文、日文、韩文字符间的空格；保留单词间及行内拉丁字母、数字周围的合法空格。

            • 修正标点：按文本语言规范补全缺失标点、调整错位标点、删除冗余标点。若输入仅为短语或短词，末尾请勿添加句号。

            • 剔除无实际语义的填充词、半截话、口吃及无意义口语碎片（如“嗯”“啊”“那个”“就是”“uh”“um”“you know”）。

            • 保留原意、语种与语气，不翻译或扩写内容。

            • 专有名词、数字、代码及专业术语保持不变。

            仅返回清理后的文本，无需解释、引号、前缀、备选方案或 Markdown 格式。
            """
        }
        let terms = hotwords
            .map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
            .filter { !$0.isEmpty }
        guard !terms.isEmpty else { return prompt }
        prompt += "\n\n用户常用术语热词表：" + terms.joined(separator: ", ")
        prompt += "。若识别文本中出现与热词发音相近的写法（如拼读、同音、分写或连写变形），"
            + "请替换为热词原形；没有相近发音的词时不要凭空插入热词。"
            + "示例：热词表含「AGENTS.md」时，识别文本「编辑 agentsdmd 这个文件」"
            + "应改为「编辑 AGENTS.md 这个文件」。"
            + "原文中已经正确出现的热词必须原样保留，不得改写。"
        return prompt
    }

    /// 原文中出现过的热词必须保留在精修结果中（对齐 Windows RefineResultKeepsHotwords）。
    static func resultKeepsHotwords(original: String, refined: String, hotwords: [String]) -> Bool {
        for hotword in hotwords where !hotword.isEmpty {
            guard original.contains(hotword) else { continue }
            if !refined.contains(hotword) { return false }
        }
        return true
    }
}
