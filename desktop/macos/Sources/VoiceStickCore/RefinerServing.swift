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

/// 精修完成的路径归因（B1 打点：写入 VoiceStickApp.log refine complete 行的 fallback 字段）。
public enum RefineFallback: String {
    /// SSE 流式链路产出（含本地请求构造即败的 ok=false）。
    case none = "none"
    /// SSE 失败后回退非流式链路产出（成败由 ok 位表达）。
    case sseToBlocking = "sse_to_blocking"
    /// 协调器预算超时回退原文：只打 "refine budget exceeded" 行，迟到的 complete 被
    /// RefineCancelToken 守卫丢弃，complete 行不会携带此值。
    case budget = "budget"
    /// 热词守卫拦截回退原文（协调器在 ok 但热词丢失时改写）。
    case hotwordGuard = "hotword_guard"
}

public protocol RefinerServing: AnyObject {
    /// corrections 非空时在精修 prompt 末尾追加用户历史纠正 few-shot 段
    /// （用户数据飞轮 P0-1，调用方传入按 count/新近度裁剪后的 top-N）。
    func refineStream(_ text: String, promptOverride: String, hotwords: [String]?,
                      corrections: [CorrectionPair],
                      cancel: RefineCancelToken,
                      onToken: @escaping (String) -> Void,
                      onComplete: @escaping (Bool, String, RefineFallback) -> Void)
}


/// 精修结果是否保住热词（原 LLMRefinementClient.resultKeepsHotwords 静态工具，纯文本判定，随协调器引用迁 Core）。
public func resultKeepsHotwords(original: String, refined: String, hotwords: [String]) -> Bool {
    for hotword in hotwords where !hotword.isEmpty {
        guard original.contains(hotword) else { continue }
        if !refined.contains(hotword) { return false }
    }
    return true
}
