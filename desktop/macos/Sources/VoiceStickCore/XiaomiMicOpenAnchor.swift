import Foundation

/// F5 抑制锚点：记录最近一次小米遥控器开麦迹象的单调毫秒时间戳。
/// 线程现状：写入（BleCentral 的 CoreBluetooth 回调 queue=.main）与读取
///（F5 tap 回调挂在主 RunLoop）同在主线程；NSLock 为防御性保留，无害。
/// 消费侧配合 `XiaomiAtvvSession.shouldSuppressF5(nowMs:lastMicOpenMs:enabled:)`：
/// `value` 为 0 表示从未开麦，等价于 lastMicOpenMs 传 nil。
public final class XiaomiMicOpenAnchor {
    private let lock = NSLock()
    private var timestampMs: Int64 = 0

    public init() {}

    /// 单调毫秒时钟（ProcessInfo.systemUptime，CLOCK_MONOTONIC 语义）：与 BleCentral
    /// 写入锚点同一基准，消费侧（F5 suppressor）用它取当前时间。
    public static func nowMs() -> Int64 {
        Int64(ProcessInfo.processInfo.systemUptime * 1000)
    }

    /// 记录开麦迹象时刻（Control 收 0x08/0x04，或任一 Audio 帧到达）。
    /// nowMs 须与 ATVV 会话同一单调时钟基准（见 BleCentral.nowMs）。
    public func note(_ nowMs: Int64) {
        lock.lock()
        timestampMs = nowMs
        lock.unlock()
    }

    /// 最近开麦时刻（单调毫秒）；从未开麦为 0。
    public var value: Int64 {
        lock.lock()
        defer { lock.unlock() }
        return timestampMs
    }
}
