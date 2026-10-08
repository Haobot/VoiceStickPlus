import Foundation

// N1 切5 闸3：精修出口协议（原 LLMRefinementClient 的 refineStream——协调器唯一调用）。
// RefineCancelToken 随迁（纯 Foundation 锁标志）；具体客户端依赖 AppConfig 故留 App
// 合规；协调器持协议、makeRefiner 闭包注入。refine(非流式)协调器未用、不入协议。
public final class RefineCancelToken {
    private let lock = NSLock()
    private var cancelled = false

    public init() {}

    public var isCancelled: Bool {
        lock.lock()
        defer { lock.unlock() }
        return cancelled
    }

    public func cancel() {
        lock.lock()
        defer { lock.unlock() }
        cancelled = true
    }
}

public protocol RefinerServing: AnyObject {
    func refineStream(_ text: String, promptOverride: String, hotwords: [String]?,
                      cancel: RefineCancelToken,
                      onToken: @escaping (String) -> Void,
                      onComplete: @escaping (Bool, String) -> Void)
}


/// 精修结果是否保住热词（原 LLMRefinementClient.resultKeepsHotwords 静态工具，纯文本判定，随协调器引用迁 Core）。
public func resultKeepsHotwords(original: String, refined: String, hotwords: [String]) -> Bool {
    for hotword in hotwords where !hotword.isEmpty {
        guard original.contains(hotword) else { continue }
        if !refined.contains(hotword) { return false }
    }
    return true
}
