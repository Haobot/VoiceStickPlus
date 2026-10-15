import Foundation
import VoiceStickCore

// 遥控器长按连发状态机（macOS HID 拦截层 inject 路径的核心节拍逻辑）：
// 「delay 后每 interval 一发、抬起即停、重按重置」，对齐系统键盘自动重复手感；
// 非 supportsRepeat 按钮不登记；断链无抬起由 maxHoldMs 兜底清除。
func runRemoteButtonRepeaterTests() {
    var repeater = RemoteButtonRepeater(timing: .init(delayMs: 100, intervalMs: 50))

    // 非 repeatable 按钮：不登记、不连发（ok/home/menu/tv/语音键双击）。
    check(!repeater.press(.ok, atMs: 0), "rr: ok 不连发登记")
    check(!repeater.press(.home, atMs: 0), "rr: home 不连发登记")
    check(!repeater.press(.voiceDoubleClick, atMs: 0), "rr: 语音键双击不经 HID 层不登记")
    check(!repeater.hasHolds, "rr: 非 repeatable 无 hold")
    check(repeater.poll(atMs: 10_000).isEmpty, "rr: 非 repeatable 永不连发")

    // repeatable 集合钉死：方向/返回/音量。
    for button in [RemoteButton.up, .down, .left, .right, .back, .volUp, .volDown] {
        check(button.supportsRepeat, "rr: \(button.rawValue) 属连发集合")
    }
    for button in [RemoteButton.ok, .home, .menu, .tv, .voiceDoubleClick] {
        check(!button.supportsRepeat, "rr: \(button.rawValue) 不属连发集合")
    }

    // delay 内不连发；到点首发的 deadline = 按下 + delay。
    check(repeater.press(.up, atMs: 0), "rr: up 登记成功")
    check(repeater.hasHolds, "rr: up 有 hold")
    check(repeater.poll(atMs: 99).isEmpty, "rr: delay 内不连发")
    check(repeater.poll(atMs: 100) == [.up], "rr: delay 到点首发")
    check(repeater.poll(atMs: 120).isEmpty, "rr: interval 内不重复")
    check(repeater.poll(atMs: 150) == [.up], "rr: 首发后按 interval 节拍（100+50）")
    check(repeater.poll(atMs: 200) == [.up], "rr: 节拍滚动（150+50）")

    // 抬起即停。
    check(repeater.release(.up), "rr: 抬起清除 hold")
    check(!repeater.release(.up), "rr: 重复抬起幂等")
    check(!repeater.hasHolds, "rr: 抬起后无 hold")
    check(repeater.poll(atMs: 1_000).isEmpty, "rr: 抬起后不再连发")

    // 短按（delay 内抬起）不连发。
    repeater.press(.back, atMs: 0)
    check(repeater.release(.back), "rr: 短按抬起")
    check(repeater.poll(atMs: 5_000).isEmpty, "rr: 短按不产生连发")

    // 重按重置计时：旧 hold 的节拍不带入新 hold。
    repeater.press(.volDown, atMs: 0)
    check(repeater.poll(atMs: 100) == [.volDown], "rr: 第一段首发")
    repeater.release(.volDown)
    repeater.press(.volDown, atMs: 200)
    check(repeater.poll(atMs: 250).isEmpty, "rr: 重按后沿用新 pressed 计时（200+100 未到）")
    check(repeater.poll(atMs: 300) == [.volDown], "rr: 重按到点首发（200+100）")

    // 多按钮并行：各自节拍、同 tick 到期一齐返回（输出确定性排序）。
    repeater.reset()
    repeater.press(.left, atMs: 0)
    repeater.press(.right, atMs: 0)
    check(repeater.poll(atMs: 100) == [.left, .right], "rr: 同 tick 到期一齐返回且排序确定")

    // 断连兜底：超 maxHoldMs 未抬起 → hold 静默清除，防无限连发。
    repeater.reset()
    repeater.press(.volUp, atMs: 0)
    check(repeater.poll(atMs: RemoteButtonRepeater.maxHoldMs - 1).isEmpty == false,
          "rr: 兜底窗口内仍连发（deadline 已过）")
    check(repeater.poll(atMs: RemoteButtonRepeater.maxHoldMs + 1_000).isEmpty,
          "rr: 超 maxHoldMs 清除 hold")
    check(!repeater.hasHolds, "rr: 兜底清除后无 hold")

    // 节拍热更：合法生效、非正忽略；更新即时反映到后续 deadline。
    repeater.setTiming(.init(delayMs: 0, intervalMs: 0))
    check(repeater.timing == .init(delayMs: 100, intervalMs: 50), "rr: 非正节拍忽略")
    repeater.setTiming(.init(delayMs: 200, intervalMs: 60))
    repeater.press(.down, atMs: 0)
    check(repeater.poll(atMs: 199).isEmpty, "rr: 新 delay 生效（未到）")
    check(repeater.poll(atMs: 200) == [.down], "rr: 新 delay 生效（到点）")

    // ButtonsSettings 默认节拍与钳位（init 与字段赋值越界均回落默认）。
    check(ButtonsSettings.default.repeatDelayMs == 300
          && ButtonsSettings.default.repeatIntervalMs == 80,
          "rr: 默认节拍 300/80")
    check(ButtonsSettings(repeatDelayMs: 5, repeatIntervalMs: 9_999).repeatDelayMs == 300
          && ButtonsSettings(repeatDelayMs: 5, repeatIntervalMs: 9_999).repeatIntervalMs == 80,
          "rr: init 越界钳位回落默认")
    check(ButtonsSettings(repeatDelayMs: 500, repeatIntervalMs: 120).repeatDelayMs == 500,
          "rr: 区间内取值保留")
}
