import Foundation
import VoiceStickCore

/// 移植 Windows TestXiaomiF5SuppressPredicate：
/// enabled 且 last>0 且 0<=age<=80ms 时吞，其余一律放行。
/// （旧 XCTest XiaomiF5PredicateTests 逐条转换。）
func runF5PredicateTests() {
    let last: Int64 = 100000

    // ---- testSuppressWindowBoundaries ----
    // 窗内（含 0/80ms 边界）吞。
    check(XiaomiAtvvSession.shouldSuppressF5(nowMs: last, lastMicOpenMs: last, enabled: true),
          "F5Suppress.age0")
    check(XiaomiAtvvSession.shouldSuppressF5(nowMs: last + 79, lastMicOpenMs: last, enabled: true),
          "F5Suppress.age79")
    check(XiaomiAtvvSession.shouldSuppressF5(
        nowMs: last + XiaomiAtvvSession.f5SuppressWindowMs, lastMicOpenMs: last, enabled: true),
        "F5Suppress.age80Boundary")
    // 窗外（81ms）放行。
    check(!XiaomiAtvvSession.shouldSuppressF5(
        nowMs: last + XiaomiAtvvSession.f5SuppressWindowMs + 1, lastMicOpenMs: last, enabled: true),
        "F5Suppress.age81Pass")

    // ---- testNilOrZeroAnchorDoesNotSuppress ----
    // 从未开麦（nil 或 0）放行。
    check(!XiaomiAtvvSession.shouldSuppressF5(nowMs: last, lastMicOpenMs: nil, enabled: true),
          "F5Suppress.nilAnchorPass")
    check(!XiaomiAtvvSession.shouldSuppressF5(nowMs: last, lastMicOpenMs: 0, enabled: true),
          "F5Suppress.zeroAnchorPass")
    // 未来时间戳（时钟回拨/乱序，age<0）放行。
    check(!XiaomiAtvvSession.shouldSuppressF5(nowMs: last - 1, lastMicOpenMs: last, enabled: true),
          "F5Suppress.futureAnchorPass")

    // ---- testDisabledDoesNotSuppress ----
    // 开关关闭放行。
    check(!XiaomiAtvvSession.shouldSuppressF5(nowMs: last, lastMicOpenMs: last, enabled: false),
          "F5Suppress.disabledPass")
}
