import AppKit
import IOKit.hid
import VoiceStickCore

/// 小米遥控器 HID 按键拦截层（macOS，二期）——薄运行时封装。
/// 方案 Doc/Plan/xiaomi-remote-button-mapping.md §4.2，经真机调试（2026-09-04）重构为
/// 「非独占 IOHIDManager(VID 0x2717) 观察驱动处置 + CGEvent tap 吞除原生事件」：
///   - 真机事实：遥控器全部按键（除语音键走 ATVV）以 keyboard page usage 装进
///     Report ID 1（3×16 位 LE usage 槽）；IOHID value 回调的 variable 元素给出
///     精确 usage（val=1 按下 / val=0 抬起），天然带设备归属（VID 匹配，看不到真键盘）。
///   - 处置挂在 IOHID 回调（而非 tap）：0xF1(back)/0x65(menu) 等键在 macOS 不产生
///     keyDown CGEvent，tap 驱动的旧架构对它们永远静默（这正是「返回键设 Delete
///     无反应」的根因之一）。
///   - inject：IOHID 按下即注入目标 KeySpec（点按式）；若该键有 macOS 原生行为
///     （方向/OK/Home/TV 产生 keyDown，音量产生 subtype=8 的 systemDefined），
///     同时记吞除标记，由 tap 按 keycode/systemKey 反映射吞掉原生事件防双触发。
///   - suppress(disabled)：只记吞除标记。
///   - native / intercept=false：什么都不做（OS 原生消费，忠实透传）。
///
/// 曾否决的备选（真机证据，详见 Doc/Plan/xiaomi-remote-button-mapping.md §4.2）：
///   - 独占 seize：已连接设备被系统 HID 栈持有，IOHIDDeviceOpen(seize) 恒
///     0xe00002c1 NotPermitted（命令行 probe 与 app 内一致），不可行。
///   - 「IOHID 锚点 + tap 关联窗」时序归因：脆弱且无必要——IOHID 观察本身已带
///     设备归因，吞除只需一次性消费标记 + 短窗兜底。
///
/// 权限：IOHID 键盘观察需「输入监控」（CGPreflightListenEventAccess），tap/注入需
/// 「辅助功能」（AXIsProcessTrusted）。两者任一缺失 → isAvailable() 为 false，
/// AppDelegate 门控不启动，仅 A 级（语音键双击）可配置；本类仅 NSLog 记日志，不弹窗。
///
/// 线程契约（对齐 XiaomiF5Suppressor）：IOHID 值回调与 tap 回调都经主 RunLoop，
/// start/stop 与吞除状态均在主线程，状态无锁。
final class XiaomiButtonInterceptManager {
    /// 遥控器 VID（IOHIDManager 匹配键）。
    private static let vendorID = 0x2717
    /// 吞除标记有效窗毫秒：覆盖 IOHID 回调 → 系统翻译 → tap 的延迟（实测音量键 1ms，
    /// 方向键/Enter 同为毫秒级；取 300ms 余量，配合一次性消费语义控制误吞面）。
    private static let suppressWindowMs: Int64 = 300
    /// systemDefined 事件的 aux-control subtype（NX_SUBTYPE_AUX_CONTROL_BUTTONS）。
    private static let auxControlSubtype = 8
    /// systemDefined data1 低位状态：0xa00=按下，0xb00=抬起（真机实测）。
    private static let auxKeyDownLow: Int64 = 0xa00
    private static let auxKeyUpLow: Int64 = 0xb00

    private let inputInjector = InputInjector()

    private var manager: IOHIDManager?
    private var tap: CFMachPort?
    private var runLoopSource: CFRunLoopSource?
    /// 当前设备按键映射（AppDelegate sync 时下发；nil=未启用）。
    private var settings: ButtonsSettings?
    /// 待吞原生事件的标记：usage → 按下时间戳（一次性消费；超时兜底失效）。
    private var pendingSuppressByUsage: [UInt32: Int64] = [:]
    /// 正在吞的键盘序列 keycode（吞 keydown 后，自动重复与同 keycode 的 keyup 联动吞）。
    private var latchedKeyCode: Int64?
    /// 正在吞的音量 systemDefined 序列（usage；down 吞入闩，up 吞出闩——长按音量时
    /// up 可能超 300ms 窗，闩锁保证配对吞除）。
    private var latchedVolumeUsage: UInt32?

    /// 拦截层是否可用：输入监控（IOHID 键盘观察）+ 辅助功能（tap/注入）。
    static func isAvailable() -> Bool {
        CGPreflightListenEventAccess() && AXIsProcessTrusted()
    }

    /// 幂等启动：已运行时仅更新映射（配置热更），不得清吞除/闩锁状态（对齐 F5 start）。
    /// 权限缺失只 NSLog 放弃（权限引导在按键映射窗/Onboarding）。
    func start(settings: ButtonsSettings) {
        self.settings = settings
        guard tap == nil else { return }
        guard Self.isAvailable() else {
            NSLog("XiaomiButtonInterceptManager: input-monitoring or accessibility permission missing; " +
                  "interception off (A-level only)")
            return
        }
        startIOHID()
        installTap()
        NSLog("XiaomiButtonInterceptManager: started (settings.intercept=\(settings.intercept))")
    }

    func stop() {
        settings = nil
        pendingSuppressByUsage = [:]
        latchedKeyCode = nil
        latchedVolumeUsage = nil
        if let runLoopSource {
            CFRunLoopRemoveSource(CFRunLoopGetMain(), runLoopSource, .commonModes)
        }
        if let tap {
            CGEvent.tapEnable(tap: tap, enable: false)
            CFMachPortInvalidate(tap)
        }
        runLoopSource = nil
        tap = nil
        if let manager {
            IOHIDManagerUnscheduleFromRunLoop(manager, CFRunLoopGetMain(), CFRunLoopMode.commonModes.rawValue)
            IOHIDManagerClose(manager, IOOptionBits(kIOHIDOptionsTypeNone))
        }
        self.manager = nil
    }

    deinit {
        stop()
    }

    // MARK: - IOHID 观察（非独占，VID 0x2717；处置驱动方）

    private func startIOHID() {
        guard manager == nil else { return }
        let mgr = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
        let match: [[String: Any]] = [[kIOHIDVendorIDKey: Self.vendorID]]
        IOHIDManagerSetDeviceMatchingMultiple(mgr, match as CFArray)
        let rc = IOHIDManagerOpen(mgr, IOOptionBits(kIOHIDOptionsTypeNone))
        guard rc == kIOReturnSuccess else {
            NSLog("XiaomiButtonInterceptManager: IOHIDManagerOpen failed 0x%08x", rc)
            return
        }
        IOHIDManagerRegisterInputValueCallback(mgr, Self.inputValueCallback,
                                               Unmanaged.passUnretained(self).toOpaque())
        IOHIDManagerScheduleWithRunLoop(mgr, CFRunLoopGetMain(), CFRunLoopMode.commonModes.rawValue)
        manager = mgr
        NSLog("XiaomiButtonInterceptManager: IOHIDManager observing VID 0x%04x", Self.vendorID)
    }

    private static let inputValueCallback: IOHIDValueCallback = { context, _, _, value in
        guard let context else { return }
        let manager = Unmanaged<XiaomiButtonInterceptManager>.fromOpaque(context).takeUnretainedValue()
        manager.handleIOHIDValue(value)
    }

    /// IOHID 值回调（仅来自遥控器，VID 已过滤）。真机报告解析结论：
    /// 每个按键报告触发 3 个元素回调——两个 array 元素（usage=0xFFFFFFFF，槽位打包值）
    /// 与一个 variable 元素（usage=精确键码，val=1 按下 / val=0 抬起）；只认 variable 元素。
    /// page 过滤 0x07（keyboard page）——vendor page 0xFF00 的 OTA/ATVV 数据流也走
    /// 同一设备，不过滤会把数据字节误判为按键。
    private func handleIOHIDValue(_ value: IOHIDValue) {
        let element = IOHIDValueGetElement(value)
        let usage = IOHIDElementGetUsage(element)
        guard usage != 0xFFFF_FFFF,
              IOHIDElementGetUsagePage(element) == 0x07 else { return }
        guard IOHIDValueGetIntegerValue(value) != 0 else { return }  // 抬起无需处置
        NSLog("XiaomiButtonInterceptManager: button down usage=0x%x", usage)
        handleRemoteButtonDown(usage: usage)
    }

    /// 遥控器按键按下处置（IOHID 驱动）：
    ///   - native / 未映射（含 power/voice F5）→ 无操作，OS 原生消费；
    ///   - disabled → 记吞除标记（tap 吞原生事件）；
    ///   - key → 立即注入目标 KeySpec + 记吞除标记；KeySpec 解析失败回落 native
    ///     （不注入也不吞，对齐旧语义）。
    private func handleRemoteButtonDown(usage: UInt32) {
        guard let settings,
              let button = RemoteButtonHIDMap.button(forUsage: usage) else { return }
        let mapping = settings.mapping(for: button)
        switch RemoteButtonHIDMap.decision(intercept: settings.intercept, mapping: mapping) {
        case .native:
            return
        case .suppress:
            pendingSuppressByUsage[usage] = XiaomiMicOpenAnchor.nowMs()
            NSLog("XiaomiButtonInterceptManager: suppress \(button.rawValue)")
        case .inject(let keyText):
            guard let spec = KeySpec.parse(keyText) else {
                NSLog("XiaomiButtonInterceptManager: invalid key \"\(keyText)\" for \(button.rawValue) -> pass native")
                return
            }
            pendingSuppressByUsage[usage] = XiaomiMicOpenAnchor.nowMs()
            NSLog("XiaomiButtonInterceptManager: inject \(button.rawValue) -> \(spec.displayText)")
            inputInjector.sendKeyCombo(spec)
        }
    }

    /// 吞除标记查询（一次性消费 + 短窗兜底）：命中即清除并返回 true。
    private func consumePendingSuppress(usage: UInt32) -> Bool {
        guard let atMs = pendingSuppressByUsage[usage] else { return false }
        guard XiaomiMicOpenAnchor.nowMs() - atMs <= Self.suppressWindowMs else {
            pendingSuppressByUsage.removeValue(forKey: usage)
            return false
        }
        pendingSuppressByUsage.removeValue(forKey: usage)
        return true
    }

    // MARK: - CGEvent tap（只吞除原生事件，不做处置）

    private func installTap() {
        guard tap == nil, AXIsProcessTrusted() else { return }
        // mask 含 NX_SYMDEFINED(14)（音量键的原生事件形态；CGEventType 未暴露该 case，
        // 用原始值）。
        let mask: CGEventMask =
            (1 << CGEventType.keyDown.rawValue) | (1 << CGEventType.keyUp.rawValue)
            | (1 << 14)
        guard let t = CGEvent.tapCreate(
            tap: .cghidEventTap,
            place: .headInsertEventTap,
            options: .defaultTap,
            eventsOfInterest: mask,
            callback: Self.tapCallback,
            userInfo: Unmanaged.passUnretained(self).toOpaque()
        ) else {
            NSLog("XiaomiButtonInterceptManager: CGEvent.tapCreate failed")
            return
        }
        let source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, t, 0)
        CFRunLoopAddSource(CFRunLoopGetMain(), source, .commonModes)
        CGEvent.tapEnable(tap: t, enable: true)
        tap = t
        runLoopSource = source
        NSLog("XiaomiButtonInterceptManager: event tap installed")
    }

    private static let tapCallback: CGEventTapCallBack = { _, type, event, userInfo in
        guard let userInfo else { return Unmanaged.passRetained(event) }
        let manager = Unmanaged<XiaomiButtonInterceptManager>.fromOpaque(userInfo).takeUnretainedValue()
        return manager.handleEvent(type: type, event: event)
    }

    /// 返回 nil = 吞掉事件；passRetained(event) = 放行。
    private func handleEvent(type: CGEventType, event: CGEvent) -> Unmanaged<CGEvent>? {
        if type == .tapDisabledByTimeout || type == .tapDisabledByUserInput {
            if let tap { CGEvent.tapEnable(tap: tap, enable: true) }
            return Unmanaged.passRetained(event)
        }
        // 音量键原生事件（subtype=8，data1=(keyCode<<16)|0xa00(down)/0xb00(up)）。
        if type.rawValue == 14 {
            return handleSystemDefinedEvent(event)
        }
        guard type == .keyDown || type == .keyUp else {
            return Unmanaged.passRetained(event)
        }
        // 非硬件来源（含我方注入）不干预：eventSourceUnixProcessID≠0（对齐 Windows LLKHF_INJECTED）。
        guard event.getIntegerValueField(.eventSourceUnixProcessID) == 0 else {
            return Unmanaged.passRetained(event)
        }
        let keyCode = event.getIntegerValueField(.keyboardEventKeycode)

        if type == .keyUp {
            if latchedKeyCode == keyCode {
                latchedKeyCode = nil
                return nil
            }
            return Unmanaged.passRetained(event)
        }

        // keyDown。
        if latchedKeyCode == keyCode {
            return nil  // 序列内自动重复
        }
        // keycode → usage 反映射（RemoteButtonHIDMap.usageToMacKeyCode 逆查）；
        // 命中吞除标记 → 吞掉原生 keyDown 并入闩（联动吞自动重复与 keyup）。
        guard let usage = RemoteButtonHIDMap.usageToMacKeyCode.first(where: { $0.value == keyCode })?.key,
              consumePendingSuppress(usage: usage) else {
            return Unmanaged.passRetained(event)  // 真实键盘/未拦截键 → 放行
        }
        latchedKeyCode = keyCode
        return nil
    }

    /// 音量键 systemDefined 吞除：down 命中吞除标记 → 吞并入闩；up 配闩吞出闩。
    /// 未命中一律放行（真实键盘音量键/OS 自身音量事件不受影响）。
    private func handleSystemDefinedEvent(_ event: CGEvent) -> Unmanaged<CGEvent>? {
        guard event.getIntegerValueField(.eventSourceUnixProcessID) == 0,
              let nsEvent = NSEvent(cgEvent: event),
              Int(nsEvent.subtype.rawValue) == Self.auxControlSubtype else {
            return Unmanaged.passRetained(event)
        }
        let keyCode = Int64(nsEvent.data1 >> 16)
        let low = Int64(nsEvent.data1 & 0xFF00)
        guard let usage = RemoteButtonHIDMap.volumeSystemKeyToUsage[keyCode] else {
            return Unmanaged.passRetained(event)  // 非音量键（播放/静音等）→ 放行
        }
        if low == Self.auxKeyDownLow {
            guard latchedVolumeUsage == nil, consumePendingSuppress(usage: usage) else {
                if latchedVolumeUsage == usage { return nil }  // 序列内重复 down
                return Unmanaged.passRetained(event)
            }
            latchedVolumeUsage = usage
            return nil
        }
        if low == Self.auxKeyUpLow, latchedVolumeUsage == usage {
            latchedVolumeUsage = nil
            return nil
        }
        return Unmanaged.passRetained(event)
    }
}
