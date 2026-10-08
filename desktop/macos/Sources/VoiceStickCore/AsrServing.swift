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
    public var resultType: ASRResultType = .full
    public var showUtterances: Bool = false

    public init(hotwords: [String] = [], resultType: ASRResultType = .full,
                showUtterances: Bool = false) {
        self.hotwords = hotwords
        self.resultType = resultType
        self.showUtterances = showUtterances
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
