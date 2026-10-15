import Foundation

// N1 切5 步4 闸1：ASR 出口四件套下沉——VoiceStickCoordinator 持有 any ASRClient，
// 协议与两个数据类型随迁 Core 后，类离 Core 仅剩：ASR 工厂注入（factory 构造 App 实现
// 故留 App）、LLMTranslationClient（仅 1 处调用→闭包化）、两个 AppKit 方法的私有链
//（statusController/recoverFromASRError 等与步4 public 化一并处理）、public 化与整迁。
// 具体客户端（ASRWebSocket/TencentASR）留 App 合规。

/// ASR 结果形态（原 ASRWebSocketClient.swift 顶部）。
public enum ASRResultType: String {
    case full
    case single
}

/// 会话参数（原 ASRWebSocketClient.swift；字段默认值即构造默认）。
public struct ASRSessionOptions {
    public var hotwords: [String] = []
    /// 最近精修文本对话上下文（用户数据飞轮 P1-6：火山 corpus.context.dialog_ctx，
    /// 让同音纠错在第一遍发生；调用方负责总量预算）。
    public var dialogContext: [String] = []
    public var resultType: ASRResultType = .full
    public var showUtterances: Bool = false

    public init(hotwords: [String] = [], dialogContext: [String] = [],
                resultType: ASRResultType = .full, showUtterances: Bool = false) {
        self.hotwords = hotwords
        self.dialogContext = dialogContext
        self.resultType = resultType
        self.showUtterances = showUtterances
    }
}

/// 火山 bigmodel corpus.context 直传载荷（纯函数，双端对拍口径）：
/// `{"hotwords":[{"word":..}],"dialog_ctx":[..]}`，两段皆空返回 nil（不带 corpus）。
/// 预算约束（kHotwordCorpusTokenBudget 直传热词 80 tokens；dialog_ctx 800 tokens/20 轮）
/// 由调用方负责，本函数只做序列化与空白过滤。
public enum ASRCorpus {
    public static func contextJSON(hotwords: [String], dialogContext: [String]) -> String? {
        var context: [String: Any] = [:]
        let words = hotwords
            .map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
            .filter { !$0.isEmpty }
        if !words.isEmpty {
            context["hotwords"] = words.map { ["word": $0] }
        }
        let dialog = dialogContext
            .map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
            .filter { !$0.isEmpty }
        if !dialog.isEmpty {
            context["dialog_ctx"] = dialog
        }
        guard !context.isEmpty,
              let data = try? JSONSerialization.data(withJSONObject: context) else { return nil }
        return String(data: data, encoding: .utf8)
    }
}

/// 一段 ASR 确定/部分结果（原 ASRWebSocketClient.swift）。
public struct ASRSegment {
    public let text: String
    public let definite: Bool
    public let startTime: Int?
    public let endTime: Int?

    public init(text: String, definite: Bool, startTime: Int?, endTime: Int?) {
        self.text = text
        self.definite = definite
        self.startTime = startTime
        self.endTime = endTime
    }
}

/// ASR 客户端出口（原 ASRWebSocketClient.swift；实现类留 App）。
public protocol ASRClient: AnyObject {
    var onPartial: ((String) -> Void)? { get set }
    var onSegment: ((ASRSegment) -> Void)? { get set }
    var onFinal: ((String) -> Void)? { get set }
    var onError: ((String) -> Void)? { get set }
    var onUpgradeURL: ((URL, String) -> Void)? { get set }

    func start(options: ASRSessionOptions) -> Bool
    func sendOggOpusChunk(_ data: Data, isLast: Bool)
    func finish()
    func cancel()
}
