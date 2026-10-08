import Foundation

// N1 切5 闸4：翻译出口协议（原 LLMTranslationClient 面——协调器单调用 translate）。
// 具体客户端依赖 AppConfig（根仍在 App）故留 App 合规；协调器持协议、makeTranslator
// 闭包注入（同 makeAsr 模式）。
public protocol TranslatorServing: AnyObject {
    func translate(_ text: String, targetLanguage: String, hotwords: [String],
                   completion: @escaping (Result<String, Error>) -> Void)
}
