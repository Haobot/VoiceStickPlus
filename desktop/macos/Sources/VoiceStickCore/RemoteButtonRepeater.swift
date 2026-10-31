import Foundation

/// 遥控器长按连发状态机（纯逻辑：无定时器/IO，poll 驱动，供单测）。节拍模型对齐
/// Windows XiaomiTapDirectKeys.PollRepeat：按下时单发注入由调用方完成（保持点按
/// 手感），本类型只负责「delay 后每 interval 一发」直至抬起；重按重新计时。
///
/// 断连兜底：BLE HID 断链不发抬起沿，超 maxHoldMs 未抬起的 hold 视为僵尸静默清除，
/// 防断连后无限连发。
public struct RemoteButtonRepeater {
    /// 连发节拍：首次重复延迟与重复间隔（毫秒），均须为正（非正忽略更新）。
    public struct Timing: Equatable {
        public var delayMs: Int
        public var intervalMs: Int

        public init(delayMs: Int, intervalMs: Int) {
            self.delayMs = delayMs
            self.intervalMs = intervalMs
        }
    }

    /// 单次 hold 最长连发时长：远超任何合理按住时长，仅兜底「断链无抬起」。
    public static let maxHoldMs: Int64 = 30_000

    private struct Hold {
        let pressedMs: Int64
        var lastFireMs: Int64?
    }

    private var holds: [RemoteButton: Hold] = [:]
    public private(set) var timing: Timing

    public init(timing: Timing) {
        self.timing = timing
    }

    /// 热更节拍（配置热更路径）；非正值忽略，保持旧节拍。
    public mutating func setTiming(_ timing: Timing) {
        guard timing.delayMs > 0, timing.intervalMs > 0 else { return }
        self.timing = timing
    }

    /// 按下登记；仅 supportsRepeat 按钮登记（返回 false = 该键不连发）。
    @discardableResult
    public mutating func press(_ button: RemoteButton, atMs nowMs: Int64) -> Bool {
        guard button.supportsRepeat else { return false }
        holds[button] = Hold(pressedMs: nowMs, lastFireMs: nil)
        return true
    }

    /// 抬起清除；返回此前是否在连发登记中（供日志区分真抬起/无关键）。
    @discardableResult
    public mutating func release(_ button: RemoteButton) -> Bool {
        holds.removeValue(forKey: button) != nil
    }

    /// 是否有未抬起的 hold（驱动方据此开关连发定时器）。
    public var hasHolds: Bool { !holds.isEmpty }

    /// 轮询到点连发：返回本 tick 到期的按钮（每按钮至多一发，发后记 lastFireMs）。
    /// 到期判定基于绝对时刻（deadline = 按下+delay 或 上发+interval），定时器粒度
    /// 抖动只影响相位不累积误差。输出按 rawValue 排序保证确定性（测试友好）。
    public mutating func poll(atMs nowMs: Int64) -> [RemoteButton] {
        var due: [RemoteButton] = []
        var expired: [RemoteButton] = []
        for (button, hold) in holds {
            if nowMs - hold.pressedMs >= Self.maxHoldMs {
                expired.append(button)
                continue
            }
            let deadline: Int64 = hold.lastFireMs.map { $0 + Int64(timing.intervalMs) }
                ?? hold.pressedMs + Int64(timing.delayMs)
            if nowMs >= deadline {
                due.append(button)
            }
        }
        for button in expired { holds.removeValue(forKey: button) }
        for button in due { holds[button]?.lastFireMs = nowMs }
        return due.sorted { $0.rawValue < $1.rawValue }
    }

    /// 清空全部 hold（拦截层 stop/重建时调用，防跨会话残留）。
    public mutating func reset() {
        holds.removeAll()
    }
}
