import AppKit
import VoiceStickCore

/// 小米遥控器 F5 抑制（语义对齐 Windows VoiceF5Suppressor，机制换成 CGEvent tap）：
/// 遥控器语音键按下时，固件除发 ATVV 开麦帧外还经 OS 级 HID 通道连带发 F5
/// （按住期间 ~30ms 自动重复；F5 可能比 ATVV 帧先到 ~1ms）。吞判定三层：
/// 1. 近窗命中：最近 80ms 内有开麦迹象（锚点由 BLE 层在 0x08/0x04 与每个音频帧
///    刷新，判定用 XiaomiAtvvSession.shouldSuppressF5）→ 吞，并入闩锁；
/// 2. 未命中 → 临时吞 + 延迟回放：CGEvent tap 回调不可阻塞（会被系统禁用），
///    无法像 Windows 钩内忙等 80ms，改为吞掉 keydown 后 100ms 再判：锚点已刷新
///    → 确认遥控器按键，入闩锁不回放；否则回放一个带自有标记的 F5 down+up；
/// 3. 闩锁：吞掉 keydown 后，本次序列的自动重复 keydown 与 keyup 一并吞，
///    keyup 后解闩。
/// 其余按键一律 O(1) 放行；带回放标记（eventSourceUserData 魔数）的事件放行；
/// 其他进程注入的事件（eventSourceUnixProcessID≠0，对齐 Windows LLKHF_INJECTED）放行。
/// 配置开关 xiaomi_suppress_f5 + RC 设备存在性门控在 AppDelegate.syncF5Suppressor。
/// 线程契约：tap 回调挂在主 RunLoop，start/stop 与延迟判定均在主线程，状态无锁。
final class XiaomiF5Suppressor {
    /// kVK_F5（硬编码对齐 InputInjector 的 keycode 写法，不引 Carbon）。
    private static let f5VirtualKey: Int64 = 0x60
    /// 回放事件自有标记（eventSourceUserData），tap 回调见此放行防自吞。
    private static let replayMagic: Int64 = 0x5653_5F46_35 // "VS_F5"
    /// 临时吞后的延迟再判时长（替代 Windows 钩内忙等 80ms；略大于近窗 80ms）。
    private static let replayDelay: TimeInterval = 0.1

    private var tap: CFMachPort?
    private var runLoopSource: CFRunLoopSource?
    private var anchor: XiaomiMicOpenAnchor?
    /// 本次 F5 按下序列已被吞（自动重复/keyup 联动），keyup 后解闩。
    private var latched = false
    /// 首个 keydown 已临时吞，等待 100ms 后再判（延迟回放窗口）。
    private var pendingDecision = false
    /// 延迟判定窗口内 keyup 已吞（短击场景）：命中也不入闩（序列已完结）。
    private var pendingKeyUpSwallowed = false
    /// 延迟判定代际：stop/新判定使其失效，防陈旧闭包误回放。
    private var decisionGeneration = 0
    /// 被临时吞下的 keydown 原修饰键，延迟回放时还原（否则丢 Shift/Cmd 等组合）。
    private var pendingReplayFlags: CGEventFlags = []

    /// 幂等：已运行时仅更新 anchor 引用——不得清闩锁/代际（syncF5Suppressor 在
    /// 连接集变化时会重复调 start，按住序列中途重置会漏 keyup）；重置只在真正
    /// 新建 tap 的分支做。无辅助功能权限时 NSLog 放弃（权限引导在 Onboarding，
    /// 此处不弹窗）。
    func start(anchor: XiaomiMicOpenAnchor) {
        self.anchor = anchor
        guard tap == nil else { return }
        latched = false
        pendingDecision = false
        pendingKeyUpSwallowed = false
        decisionGeneration += 1
        guard AXIsProcessTrusted() else {
            NSLog("XiaomiF5Suppressor: accessibility permission missing; F5 suppression disabled")
            return
        }
        let mask: CGEventMask =
            (1 << CGEventType.keyDown.rawValue) | (1 << CGEventType.keyUp.rawValue)
        guard let tap = CGEvent.tapCreate(
            tap: .cghidEventTap,
            place: .headInsertEventTap,
            options: .defaultTap,
            eventsOfInterest: mask,
            callback: Self.tapCallback,
            userInfo: Unmanaged.passUnretained(self).toOpaque()
        ) else {
            NSLog("XiaomiF5Suppressor: CGEvent.tapCreate failed")
            return
        }
        let source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0)
        CFRunLoopAddSource(CFRunLoopGetMain(), source, .commonModes)
        CGEvent.tapEnable(tap: tap, enable: true)
        self.tap = tap
        runLoopSource = source
        NSLog("XiaomiF5Suppressor: event tap installed")
    }

    func stop() {
        decisionGeneration += 1
        if let runLoopSource {
            CFRunLoopRemoveSource(CFRunLoopGetMain(), runLoopSource, .commonModes)
        }
        if let tap {
            CGEvent.tapEnable(tap: tap, enable: false)
            CFMachPortInvalidate(tap)
        }
        runLoopSource = nil
        tap = nil
        latched = false
        pendingDecision = false
        pendingKeyUpSwallowed = false
    }

    deinit {
        stop()
    }

    private static let tapCallback: CGEventTapCallBack = { _, type, event, userInfo in
        guard let userInfo else { return Unmanaged.passRetained(event) }
        let suppressor = Unmanaged<XiaomiF5Suppressor>.fromOpaque(userInfo).takeUnretainedValue()
        return suppressor.handleEvent(type: type, event: event)
    }

    /// 返回 nil = 吞掉事件；passRetained(event) = 放行。
    private func handleEvent(type: CGEventType, event: CGEvent) -> Unmanaged<CGEvent>? {
        // tap 被系统禁用（超时/用户输入）→ 重启用。
        if type == .tapDisabledByTimeout || type == .tapDisabledByUserInput {
            if let tap {
                CGEvent.tapEnable(tap: tap, enable: true)
            }
            return Unmanaged.passRetained(event)
        }
        guard type == .keyDown || type == .keyUp else {
            return Unmanaged.passRetained(event)
        }
        // 只处理 F5，其余按键 O(1) 放行。
        guard event.getIntegerValueField(.keyboardEventKeycode) == Self.f5VirtualKey else {
            return Unmanaged.passRetained(event)
        }
        // 自有回放事件放行。
        guard event.getIntegerValueField(.eventSourceUserData) != Self.replayMagic else {
            return Unmanaged.passRetained(event)
        }
        // 其他进程注入的按键不干预（对齐 Windows LLKHF_INJECTED 语义）：HID 硬件
        // 来源 pid 为 0，CGEventPost 注入的事件带投递方 pid。
        guard event.getIntegerValueField(.eventSourceUnixProcessID) == 0 else {
            return Unmanaged.passRetained(event)
        }

        if type == .keyUp {
            // 键程关联（对齐 Windows）：仅本次序列被吞时才吞抬起；松开阶段音频流
            // 已停、80ms 窗可能已过期，没有关联 keyup 会漏。
            if latched {
                latched = false
                return nil
            }
            if pendingDecision {
                // 延迟判定窗口内的短击抬起：吞掉（回放会补完整 down+up），
                // 并标记——随后判定即使命中也不入闩（序列已完结）。
                pendingKeyUpSwallowed = true
                return nil
            }
            return Unmanaged.passRetained(event)
        }

        // keyDown：闩锁内（自动重复）或延迟判定窗口内的重复直接吞。
        if latched || pendingDecision {
            return nil
        }

        let now = XiaomiMicOpenAnchor.nowMs()
        if XiaomiAtvvSession.shouldSuppressF5(nowMs: now, lastMicOpenMs: anchor?.value, enabled: true) {
            latched = true
            NSLog("f5 keydown mic_open_age_ms=\(now - (anchor?.value ?? 0)) -> suppress")
            return nil
        }

        // 未命中：临时吞 + 100ms 后再判（F5 可能先于 ATVV 帧到达）。
        pendingDecision = true
        pendingKeyUpSwallowed = false
        pendingReplayFlags = event.flags
        decisionGeneration += 1
        let generation = decisionGeneration
        DispatchQueue.main.asyncAfter(deadline: .now() + Self.replayDelay) { [weak self] in
            self?.resolvePendingF5(generation: generation)
        }
        return nil
    }

    /// 延迟再判：此刻近窗命中（锚点已被 BLE 层刷新）→ 确认遥控器按键，入闩锁
    /// 不回放；否则经 .cgHIDEventTap 回放一个带标记的 F5 down+up。
    private func resolvePendingF5(generation: Int) {
        guard generation == decisionGeneration, pendingDecision else { return }
        pendingDecision = false
        let now = XiaomiMicOpenAnchor.nowMs()
        if XiaomiAtvvSession.shouldSuppressF5(nowMs: now, lastMicOpenMs: anchor?.value, enabled: true) {
            latched = !pendingKeyUpSwallowed
            pendingKeyUpSwallowed = false
            NSLog("f5 keydown -> suppress (delayed hit)")
            return
        }
        pendingKeyUpSwallowed = false
        NSLog("f5 keydown -> pass (replayed after \(Int(Self.replayDelay * 1000))ms)")
        replayF5()
    }

    /// 回放一次完整 F5 点按（带自有标记，tap 回调见此放行）；修饰键还原被吞
    /// keydown 的原 flags（否则 Shift+F5 等组合回放后丢修饰）。
    private func replayF5() {
        guard let source = CGEventSource(stateID: .hidSystemState) else { return }
        let keyCode = CGKeyCode(Self.f5VirtualKey)
        let down = CGEvent(keyboardEventSource: source, virtualKey: keyCode, keyDown: true)
        let up = CGEvent(keyboardEventSource: source, virtualKey: keyCode, keyDown: false)
        down?.setIntegerValueField(.eventSourceUserData, value: Self.replayMagic)
        up?.setIntegerValueField(.eventSourceUserData, value: Self.replayMagic)
        down?.flags = pendingReplayFlags
        up?.flags = pendingReplayFlags
        down?.post(tap: .cghidEventTap)
        up?.post(tap: .cghidEventTap)
    }
}
