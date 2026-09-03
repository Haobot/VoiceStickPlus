import AppKit
import VoiceStickCore

/// 电池电压监测窗口（对齐 Windows BatteryMonitorDialog + PowerLogMonitor）。
///
/// 状态机：idle → anchoring（clear → 400ms → time_anchor → 400ms）→ probing
/// （dump offset=1e9 越界探测，设备钳到 total 回 eof 空片）→ monitoring
/// （60 周期 × 60s，周期内 dump offset=log_size max=160，重试 3 次，连续失败
/// 3 周期中止）→ finished / error。Stop 回 idle（已采数据保留可导出）。
///
/// 接线：onSendCommand(deviceID, payload) 下发 control_rx（调用方路由到
/// BleCentral.sendPowerLogCommand）；fragment/power_mgmt 回调由 AppDelegate 从
/// BleCentral 透传（仅当本窗口设备匹配）。
final class BatteryMonitorWindowController: NSWindowController, NSWindowDelegate {

    // MARK: - 常量（对齐 Windows battery_monitor_dialog.h）

    private static let probeOffset: UInt32 = 1_000_000_000
    private static let dumpChunkMax: UInt32 = 160
    private static let probeDumpMax: UInt32 = 16
    private static let totalCycles = 60
    private static let cycleInterval: TimeInterval = 60
    private static let anchorStepInterval: TimeInterval = 0.4
    private static let probeTimeout: TimeInterval = 20
    private static let dumpTimeout: TimeInterval = 20
    private static let maxAttemptsPerCycle = 3
    private static let maxFailedCycles = 3

    // MARK: - 日志条目与累积器（对齐 Windows ParsePowerLogEntry/PowerLogAccumulator）

    struct PowerLogSample {
        let uptimeS: UInt32
        let vbatMv: Int
        let mode: UInt8
        let charging: Bool
        let usbPowered: Bool
        /// 锚点对齐后的墙钟 epoch 秒；无有效锚点时为 -1。
        let epochS: Int64

        var isValid: Bool { vbatMv > 0 }
    }

    /// 增量日志累积器：12 字节条目流（不含 16 字节头），两遍处理——第一遍完整
    /// 解析 + 重启检测（任何非锚点条目 uptime 回退 → 整体失败且内部状态不变），
    /// 第二遍产出采样（非锚点 + periodic 标志 + uptime 递增）。
    final class PowerLogAccumulator {
        private(set) var samples: [PowerLogSample] = []
        private var hasAnchor = false
        private var anchorEpochS: Int64 = 0
        private var anchorUptimeS: UInt32 = 0
        private var lastUptimeS: UInt32 = 0

        private struct ParsedEntry {
            let uptimeS: UInt32
            let vbatMv: Int
            let mode: UInt8
            let flags: UInt8
            let anchorEpoch: UInt32
            var isAnchor: Bool { flags & 0x10 != 0 }
            var isPeriodic: Bool { flags & 0x04 != 0 }
        }

        /// 消费一段增量 blob（长度必须 12 的倍数）。返回 false 表示流损坏或设备重启。
        func consumeIncrementalBlob(_ blob: Data) -> Bool {
            guard blob.count % 12 == 0 else { return false }
            // 第一遍：完整解析 + 重启检测（内部状态不变）。
            var entries: [ParsedEntry] = []
            entries.reserveCapacity(blob.count / 12)
            for offset in stride(from: 0, to: blob.count, by: 12) {
                let entry = Self.parseEntry(blob, at: offset)
                if !entry.isAnchor && entry.uptimeS < lastUptimeS {
                    return false
                }
                entries.append(entry)
            }
            // 第二遍：锚点更新基准（多锚点取 uptime 最大者），周期条目产出采样。
            var maxSampledUptime = lastUptimeS
            for entry in entries {
                if entry.isAnchor {
                    if !hasAnchor || entry.uptimeS > anchorUptimeS {
                        hasAnchor = true
                        anchorEpochS = Int64(entry.anchorEpoch)
                        anchorUptimeS = entry.uptimeS
                    }
                    continue
                }
                guard entry.isPeriodic, entry.uptimeS > lastUptimeS else { continue }
                let epochS: Int64 = hasAnchor && anchorUptimeS <= entry.uptimeS
                    ? anchorEpochS + Int64(entry.uptimeS &- anchorUptimeS)
                    : -1
                samples.append(PowerLogSample(
                    uptimeS: entry.uptimeS,
                    vbatMv: entry.vbatMv,
                    mode: entry.mode,
                    charging: entry.flags & 0x01 != 0,
                    usbPowered: entry.flags & 0x02 != 0,
                    epochS: epochS
                ))
                maxSampledUptime = max(maxSampledUptime, entry.uptimeS)
            }
            lastUptimeS = maxSampledUptime
            return true
        }

        /// 12 字节条目（全小端）：uptime_s u32 | vbat_mv u16 | mode u8 | flags u8 | anchor_epoch u32。
        private static func parseEntry(_ blob: Data, at offset: Int) -> ParsedEntry {
            let bytes = [UInt8](blob[offset..<(offset + 12)])
            let uptime = UInt32(bytes[0]) | UInt32(bytes[1]) << 8 |
                UInt32(bytes[2]) << 16 | UInt32(bytes[3]) << 24
            let vbatMv = Int(UInt16(bytes[4]) | UInt16(bytes[5]) << 8)
            let anchorEpoch = UInt32(bytes[8]) | UInt32(bytes[9]) << 8 |
                UInt32(bytes[10]) << 16 | UInt32(bytes[11]) << 24
            return ParsedEntry(
                uptimeS: uptime, vbatMv: vbatMv, mode: bytes[6],
                flags: bytes[7], anchorEpoch: anchorEpoch
            )
        }
    }

    // MARK: - 状态机

    private enum MonitorState {
        case idle
        case anchoring
        case probing
        case monitoring
        case finished
        case error(String)
    }

    private enum AbortReason {
        case probeTimeout
        case dumpTimeoutRepeated
        case restarted
        case disconnected
        case logCleared

        var message: String {
            switch self {
            case .probeTimeout: return tr(.batteryErrProbeTimeout)
            case .dumpTimeoutRepeated: return tr(.batteryErrDumpTimeout)
            case .restarted: return tr(.batteryErrRestart)
            case .disconnected: return tr(.batteryErrDisconnected)
            case .logCleared: return tr(.batteryErrLogCleared)
            }
        }
    }

    private let onSendCommand: (Data) -> Void
    /// 目标设备 ID（AppDelegate 断连通知与分片路由用）。
    let deviceID: String
    /// 监测中关窗 = 会话静默终止（对齐 Windows：无确认、主机不发收尾命令）。
    var onClosed: (() -> Void)?

    private var state: MonitorState = .idle
    /// 累积器（Start 时重新构造清零，对齐 Windows StartMonitoring 重置语义）。
    private var accumulator = PowerLogAccumulator()
    private var logSize: UInt32 = 0
    private var cycle = 0
    private var attempt = 0
    private var failedCycles = 0
    private var dumpActive = false
    private var pendingBlob = Data()
    private var expectedOffset: UInt32 = 0
    /// usb_auto_off 显示态：固件 power_mgmt 回推落定（点击只下发不本地落定）。
    private var usbAutoOff = false
    /// 保存结果等覆盖状态文案（仅非 monitoring 态优先显示）。
    private var statusOverride: String?

    // 5 个定时器（对齐 Windows：anchor_step/probe/dump/cycle/tick）。
    private var anchorStepTimer: Timer?
    private var probeTimeoutTimer: Timer?
    private var dumpTimeoutTimer: Timer?
    private var cycleTimer: Timer?
    private var tickTimer: Timer?
    private var anchorStep = 0
    /// 倒计时显示锚点（下一周期触发时刻）。
    private var nextCycleAt: Date?

    // MARK: - UI 控件

    private let statusLabel = NSTextField(wrappingLabelWithString: "")
    private let usbAutoOffButton = NSButton(
        checkboxWithTitle: "", target: nil, action: nil
    )
    private let warnLabel = NSTextField(wrappingLabelWithString: "")
    private let startButton = NSButton(title: "", target: nil, action: nil)
    private let stopButton = NSButton(title: "", target: nil, action: nil)
    private let exportCSVButton = NSButton(title: "", target: nil, action: nil)
    private let exportPNGButton = NSButton(title: "", target: nil, action: nil)
    private let closeButton = NSButton(title: "", target: nil, action: nil)

    init(deviceID: String, onSendCommand: @escaping (Data) -> Void) {
        self.deviceID = deviceID
        self.onSendCommand = onSendCommand

        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 600, height: 160),
            styleMask: [.titled, .closable],
            backing: .buffered,
            defer: false
        )
        window.title = tr(.batteryMonitorTitle, deviceID)
        window.isReleasedWhenClosed = false
        super.init(window: window)
        window.delegate = self
        usbAutoOffButton.title = tr(.batteryUsbAutoOff)
        warnLabel.stringValue = tr(.batteryWarnUsb)
        startButton.title = tr(.batteryStart)
        stopButton.title = tr(.batteryStop)
        exportCSVButton.title = tr(.batteryExportCsv)
        exportPNGButton.title = tr(.batteryExportPng)
        closeButton.title = tr(.close)
        buildContent()
        refreshStatus()
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    func show() {
        showWindow(nil)
        window?.center()
        NSApp.activate(ignoringOtherApps: true)
    }

    private func buildContent() {
        guard let contentView = window?.contentView else { return }

        statusLabel.font = NSFont.systemFont(ofSize: NSFont.systemFontSize)
        warnLabel.textColor = .systemOrange
        warnLabel.isHidden = true

        usbAutoOffButton.target = self
        usbAutoOffButton.action = #selector(usbAutoOffToggled)
        startButton.target = self
        startButton.action = #selector(startMonitoring)
        stopButton.target = self
        stopButton.action = #selector(stopMonitoring)
        exportCSVButton.target = self
        exportCSVButton.action = #selector(exportCSV)
        exportPNGButton.target = self
        exportPNGButton.action = #selector(exportPNG)
        closeButton.target = self
        closeButton.action = #selector(closeWindow)

        // 手工 frame 布局（对齐 Windows 600x160 客户区坐标）。
        statusLabel.frame = NSRect(x: 16, y: 102, width: 340, height: 44)
        usbAutoOffButton.frame = NSRect(x: 366, y: 122, width: 218, height: 22)
        warnLabel.frame = NSRect(x: 16, y: 64, width: 568, height: 34)
        startButton.frame = NSRect(x: 16, y: 16, width: 90, height: 30)
        stopButton.frame = NSRect(x: 116, y: 16, width: 90, height: 30)
        exportCSVButton.frame = NSRect(x: 216, y: 16, width: 110, height: 30)
        exportPNGButton.frame = NSRect(x: 336, y: 16, width: 110, height: 30)
        closeButton.frame = NSRect(x: 494, y: 16, width: 90, height: 30)

        for view in [statusLabel, usbAutoOffButton, warnLabel,
                     startButton, stopButton, exportCSVButton, exportPNGButton, closeButton] {
            contentView.addSubview(view)
        }
    }

    // MARK: - 状态机驱动

    @objc private func startMonitoring() {
        // 运行中重入忽略（对齐 Windows）。
        switch state {
        case .anchoring, .probing, .monitoring: return
        default: break
        }
        logSize = 0
        cycle = 0
        attempt = 0
        failedCycles = 0
        dumpActive = false
        pendingBlob = Data()
        statusOverride = nil
        accumulator = PowerLogAccumulator()
        state = .anchoring
        anchorStep = 1
        // 先发 clear：旧固件环形区写满后对 offset==total 的增量 dump 不回包
        // （代价是丢历史记录）。
        onSendCommand(BleProtocol.powerLogClearPayload())
        scheduleAnchorStep()
        refreshStatus()
    }

    @objc private func stopMonitoring() {
        killAllTimers()
        state = .idle
        dumpActive = false
        refreshStatus()
    }

    private func scheduleAnchorStep() {
        anchorStepTimer?.invalidate()
        anchorStepTimer = Timer.scheduledTimer(
            withTimeInterval: Self.anchorStepInterval, repeats: false
        ) { [weak self] _ in
            self?.advanceAnchorStep()
        }
    }

    private func advanceAnchorStep() {
        guard case .anchoring = state else { return }
        if anchorStep == 1 {
            anchorStep = 2
            // time_anchor：当前桌面 epoch（uint32 秒）。
            onSendCommand(BleProtocol.powerLogTimeAnchorPayload(epoch: UInt32(Date().timeIntervalSince1970)))
            scheduleAnchorStep()
        } else {
            beginProbing()
        }
    }

    private func beginProbing() {
        state = .probing
        onSendCommand(BleProtocol.powerLogDumpPayload(offset: Self.probeOffset, max: Self.probeDumpMax))
        probeTimeoutTimer?.invalidate()
        probeTimeoutTimer = Timer.scheduledTimer(
            withTimeInterval: Self.probeTimeout, repeats: false
        ) { [weak self] _ in
            self?.abort(.probeTimeout)
        }
        refreshStatus()
    }

    private func beginMonitoring() {
        state = .monitoring
        cycle = 0
        failedCycles = 0
        nextCycleAt = Date().addingTimeInterval(Self.cycleInterval)
        cycleTimer?.invalidate()
        cycleTimer = Timer.scheduledTimer(
            withTimeInterval: Self.cycleInterval, repeats: true
        ) { [weak self] _ in
            self?.startCycle()
        }
        tickTimer?.invalidate()
        tickTimer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            self?.refreshStatus()
        }
        refreshStatus()
    }

    private func startCycle() {
        guard case .monitoring = state, !dumpActive else { return }
        cycle += 1
        attempt = 0
        nextCycleAt = Date().addingTimeInterval(Self.cycleInterval)
        startDumpAttempt()
    }

    private func startDumpAttempt() {
        attempt += 1
        pendingBlob = Data()
        expectedOffset = logSize
        dumpActive = true
        onSendCommand(BleProtocol.powerLogDumpPayload(offset: logSize, max: Self.dumpChunkMax))
        dumpTimeoutTimer?.invalidate()
        dumpTimeoutTimer = Timer.scheduledTimer(
            withTimeInterval: Self.dumpTimeout, repeats: false
        ) { [weak self] _ in
            self?.handleDumpFailure()
        }
        refreshStatus()
    }

    /// power_log 分片入口（AppDelegate 透传）。probing 态等 eof 空片定基线；
    /// monitoring 态严格按 offset 顺序重组，乱序/丢片走失败路径（不做乱序缓存）。
    func handlePowerLogFragment(_ fragment: PowerLogFragment) {
        switch state {
        case .probing:
            guard fragment.eof else { return }
            probeTimeoutTimer?.invalidate()
            probeTimeoutTimer = nil
            logSize = fragment.total
            beginMonitoring()
        case .monitoring:
            guard dumpActive else { return }
            guard fragment.offset == expectedOffset else {
                handleDumpFailure()
                return
            }
            pendingBlob.append(fragment.data)
            expectedOffset &+= UInt32(fragment.data.count)
            if fragment.eof {
                dumpTimeoutTimer?.invalidate()
                dumpTimeoutTimer = nil
                finishCycle(total: fragment.total)
            }
        default:
            break
        }
    }

    private func finishCycle(total: UInt32) {
        dumpActive = false
        // total 回退 = 设备端日志被清空。
        if total < logSize {
            abort(.logCleared)
            return
        }
        // uptime 回退 = 设备重启（累积器整体拒绝且状态不变）。
        if !accumulator.consumeIncrementalBlob(pendingBlob) {
            abort(.restarted)
            return
        }
        logSize = total
        failedCycles = 0
        if cycle >= Self.totalCycles {
            killAllTimers()
            state = .finished
        }
        refreshStatus()
    }

    /// dump 失败/超时：周期内最多 3 次尝试，连续失败 3 个周期中止。
    private func handleDumpFailure() {
        dumpTimeoutTimer?.invalidate()
        dumpTimeoutTimer = nil
        dumpActive = false
        if attempt < Self.maxAttemptsPerCycle {
            startDumpAttempt()
            return
        }
        failedCycles += 1
        if failedCycles >= Self.maxFailedCycles {
            abort(.dumpTimeoutRepeated)
            return
        }
        refreshStatus()
    }

    private func abort(_ reason: AbortReason) {
        killAllTimers()
        state = .error(reason.message)
        refreshStatus()
    }

    /// 监测三态（anchoring/probing/monitoring）中设备断连 → error。
    func notifyDeviceDisconnected() {
        switch state {
        case .anchoring, .probing, .monitoring:
            abort(.disconnected)
        default:
            break
        }
    }

    /// power_mgmt 回推：落定 usb_auto_off 显示态（点击只下发，不本地落定）。
    func handlePowerMgmtEvent(_ event: PowerMgmtEvent) {
        usbAutoOff = event.usbAutoOff
        usbAutoOffButton.state = event.usbAutoOff ? .on : .off
        refreshStatus()
    }

    /// 开窗后调用方用缓存的 usb_auto_off 状态立即同步一次（对齐 Windows）。
    func setInitialUsbAutoOff(_ enabled: Bool) {
        usbAutoOff = enabled
        usbAutoOffButton.state = enabled ? .on : .off
        refreshStatus()
    }

    private func killAllTimers() {
        anchorStepTimer?.invalidate()
        anchorStepTimer = nil
        probeTimeoutTimer?.invalidate()
        probeTimeoutTimer = nil
        dumpTimeoutTimer?.invalidate()
        dumpTimeoutTimer = nil
        cycleTimer?.invalidate()
        cycleTimer = nil
        tickTimer?.invalidate()
        tickTimer = nil
    }

    // MARK: - 状态行与按钮态

    private func refreshStatus() {
        let samples = accumulator.samples
        let statusText: String
        if let override = statusOverride, !isMonitoring {
            statusOverride = nil
            statusText = override
        } else {
            switch state {
            case .idle:
                statusText = tr(.batteryStatusIdle)
            case .anchoring:
                statusText = tr(.batteryStatusAnchoring)
            case .probing:
                statusText = tr(.batteryStatusProbing)
            case .monitoring:
                let countdown: String
                if let nextCycleAt {
                    let remainMs = max(0, nextCycleAt.timeIntervalSinceNow) * 1000
                    countdown = "\(Int((remainMs + 999) / 1000))s"
                } else {
                    countdown = "..."
                }
                statusText = tr(.batteryStatusMonitoring, cycle, Self.totalCycles, samples.count, countdown)
            case .finished:
                statusText = tr(.batteryStatusFinished, samples.count)
            case .error(let message):
                statusText = tr(.batteryStatusError, message)
            }
        }
        statusLabel.stringValue = statusText

        // 按钮可用态：Start 仅非运行态；Stop 仅运行态；导出在 samples 非空时可用。
        startButton.isEnabled = !isRunning
        stopButton.isEnabled = isRunning
        exportCSVButton.isEnabled = !samples.isEmpty
        exportPNGButton.isEnabled = !samples.isEmpty

        // USB 警告行：监测运行中且（最新采样 usb_powered 或 usb_auto_off 开）。
        let latestUsbPowered = samples.last?.usbPowered ?? false
        warnLabel.isHidden = !(isRunning && (latestUsbPowered || usbAutoOff))
    }

    private var isRunning: Bool {
        switch state {
        case .anchoring, .probing, .monitoring: return true
        default: return false
        }
    }

    private var isMonitoring: Bool {
        if case .monitoring = state { return true }
        return false
    }

    // MARK: - usb_auto_off 勾选

    @objc private func usbAutoOffToggled() {
        // 只下发，不做本地落定；固件回推 power_mgmt 后 handlePowerMgmtEvent 落定。
        onSendCommand(BleProtocol.usbAutoOffPayload(enabled: usbAutoOffButton.state == .on))
    }

    // MARK: - CSV 导出（对齐 Windows FormatCsv：CRLF、无 BOM）

    private static let csvHeader = "seq,timestamp_iso,epoch_s,uptime_s,vbat_mv,vbat_v,charging,usb_powered,mode_name,valid"

    private func formatCSV() -> String {
        var text = Self.csvHeader + "\r\n"
        let formatter = DateFormatter()
        formatter.dateFormat = "yyyy-MM-dd'T'HH:mm:ss"
        for (index, sample) in accumulator.samples.enumerated() {
            let timestampISO = sample.epochS >= 0
                ? formatter.string(from: Date(timeIntervalSince1970: TimeInterval(sample.epochS)))
                : ""
            let epochText = sample.epochS >= 0 ? "\(sample.epochS)" : ""
            let vbatV = String(format: "%.3f", Double(sample.vbatMv) / 1000.0)
            text += "\(index + 1),\(timestampISO),\(epochText),\(sample.uptimeS),\(sample.vbatMv),\(vbatV),\(sample.charging ? 1 : 0),\(sample.usbPowered ? 1 : 0),\(Self.modeName(sample.mode)),\(sample.isValid ? 1 : 0)\r\n"
        }
        return text
    }

    /// mode 名（对齐 Windows ModeName；未知为 UNKNOWN_<n>）。
    private static func modeName(_ mode: UInt8) -> String {
        switch mode {
        case 0: return "S0_ACTIVE"
        case 1: return "S1_RESTING"
        case 2: return "S2_SCREEN_OFF"
        case 3: return "S3_POWER_OFF"
        case 4: return "RECORDING"
        case 5: return "ADVERTISING"
        case 6: return "OTA"
        case 0xFF: return "TIME_ANCHOR"
        default: return "UNKNOWN_\(mode)"
        }
    }

    /// 默认导出文件名：battery_VS-{id}_{YYYYMMDD_HHMMSS}.<ext>（本地时间）。
    private func defaultExportFilename(extension ext: String) -> String {
        let formatter = DateFormatter()
        formatter.dateFormat = "yyyyMMdd_HHmmss"
        return "battery_VS-\(deviceID)_\(formatter.string(from: Date())).\(ext)"
    }

    @objc private func exportCSV() {
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.commaSeparatedText]
        panel.nameFieldStringValue = defaultExportFilename(extension: "csv")
        panel.title = tr(.batteryExportCsv)
        guard panel.runModal() == .OK, let url = panel.url else { return }
        do {
            try formatCSV().write(to: url, atomically: true, encoding: .utf8)
            statusOverride = tr(.batterySavedTo, url.path)
        } catch {
            statusOverride = tr(.batterySaveFailed, error.localizedDescription)
        }
        refreshStatus()
    }

    // MARK: - PNG 导出（对齐 Windows GDI+ 布局：1000×500、网格、折线+圆点）

    @objc private func exportPNG() {
        let validSamples = accumulator.samples.filter { $0.vbatMv > 0 && $0.epochS >= 0 }
        guard validSamples.count >= 2 else {
            statusOverride = tr(.batteryNotEnoughSamples)
            refreshStatus()
            return
        }
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.png]
        panel.nameFieldStringValue = defaultExportFilename(extension: "png")
        panel.title = tr(.batteryExportPng)
        guard panel.runModal() == .OK, let url = panel.url else { return }
        do {
            let data = try renderPNG(samples: validSamples)
            try data.write(to: url)
            statusOverride = tr(.batterySavedTo, url.path)
        } catch {
            statusOverride = tr(.batterySaveFailed, error.localizedDescription)
        }
        refreshStatus()
    }

    private func renderPNG(samples: [PowerLogSample]) throws -> Data {
        let width = 1000, height = 500
        guard let context = CGContext(
            data: nil, width: width, height: height, bitsPerComponent: 8, bytesPerRow: 0,
            space: CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue
        ) else {
            throw NSError(domain: "BatteryMonitor", code: 1, userInfo: [
                NSLocalizedDescriptionKey: "PNG encode/save failed"
            ])
        }
        let nsContext = NSGraphicsContext(cgContext: context, flipped: false)
        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = nsContext
        defer { NSGraphicsContext.restoreGraphicsState() }

        // 白底。
        NSColor.white.setFill()
        NSRect(x: 0, y: 0, width: width, height: height).fill()

        // 数据域：X = 相对首点分钟（x_max 至少 1），Y = vbat_mv 上下各 pad 10%。
        let t0 = samples[0].epochS
        let xs = samples.map { Double($0.epochS - t0) / 60.0 }
        let xMax = max(xs.last ?? 1, 1.0)
        let volts = samples.map(\.vbatMv)
        let vMin = Double(volts.min() ?? 0)
        let vMax = Double(volts.max() ?? 0)
        let pad = max((vMax - vMin) * 0.1, 10)
        let yMin = vMin - pad
        let yMax = vMax + pad

        // 绘图区（CoreGraphics 左下角原点；Windows 坐标 top-left 需翻转 y）。
        let plotLeft = 80.0, plotRight = 970.0, plotTop = 56.0, plotBottom = 430.0
        let plotW = plotRight - plotLeft
        let plotH = plotBottom - plotTop
        func mapX(_ x: Double) -> Double { plotLeft + x / xMax * plotW }
        // Windows: map_y = 430 - (v-min)/(max-min)*374（top-left 原点）→ CG 翻转。
        func mapY(_ v: Double) -> Double {
            Double(height) - (plotBottom - (v - yMin) / (yMax - yMin) * plotH)
        }
        /// Windows 坐标（top-left 原点）转 CG 坐标。
        func cgY(_ windowsY: Double) -> Double { Double(height) - windowsY }

        let textColor = NSColor(calibratedRed: 30/255, green: 30/255, blue: 30/255, alpha: 1)
        let axisColor = NSColor(calibratedRed: 60/255, green: 60/255, blue: 60/255, alpha: 1)
        let gridColor = NSColor(calibratedRed: 220/255, green: 220/255, blue: 220/255, alpha: 1)
        let lineColor = NSColor(calibratedRed: 31/255, green: 119/255, blue: 180/255, alpha: 1)
        let titleFont = NSFont.boldSystemFont(ofSize: 15)
        let labelFont = NSFont.systemFont(ofSize: 12)
        let labelAttrs: [NSAttributedString.Key: Any] = [
            .font: labelFont, .foregroundColor: textColor
        ]

        // 标题（居中）：对齐 Windows kBatteryMonitorChartTitle。
        let title = tr(.batteryChartTitle, deviceID, samples.count)
        let titleAttrs: [NSAttributedString.Key: Any] = [
            .font: titleFont, .foregroundColor: textColor
        ]
        let titleSize = title.size(withAttributes: titleAttrs)
        title.draw(at: NSPoint(x: (Double(width) - titleSize.width) / 2,
                               y: cgY(12 + 24)), withAttributes: titleAttrs)

        // Y 轴网格 4 等分（5 条横线）+ 右对齐刻度标签。
        for i in 0...4 {
            let v = yMin + (yMax - yMin) * Double(i) / 4
            let y = mapY(v)
            gridColor.setStroke()
            let path = NSBezierPath()
            path.lineWidth = 1
            path.move(to: NSPoint(x: plotLeft, y: y))
            path.line(to: NSPoint(x: plotRight, y: y))
            path.stroke()
            let label = "\(Int(v + 0.5))"
            let size = label.size(withAttributes: labelAttrs)
            label.draw(at: NSPoint(x: 72 - size.width, y: y - 8), withAttributes: labelAttrs)
        }
        // X 轴 5 等分（6 条竖线）+ 居中刻度标签。
        for i in 0...5 {
            let x = xMax * Double(i) / 5
            let px = mapX(x)
            gridColor.setStroke()
            let path = NSBezierPath()
            path.lineWidth = 1
            path.move(to: NSPoint(x: px, y: cgY(plotTop)))
            path.line(to: NSPoint(x: px, y: cgY(plotBottom)))
            path.stroke()
            let label = "\(Int(x + 0.5))"
            let size = label.size(withAttributes: labelAttrs)
            label.draw(at: NSPoint(x: px - size.width / 2, y: cgY(436 + 16)), withAttributes: labelAttrs)
        }

        // 坐标轴：左竖线 + 底横线。
        axisColor.setStroke()
        let axisPath = NSBezierPath()
        axisPath.lineWidth = 1
        axisPath.move(to: NSPoint(x: plotLeft, y: cgY(plotTop)))
        axisPath.line(to: NSPoint(x: plotLeft, y: cgY(plotBottom)))
        axisPath.line(to: NSPoint(x: plotRight, y: cgY(plotBottom)))
        axisPath.stroke()

        // 轴标题。
        let xTitle = tr(.batteryAxisTime)
        let xTitleSize = xTitle.size(withAttributes: labelAttrs)
        xTitle.draw(at: NSPoint(x: plotLeft + (plotW - xTitleSize.width) / 2,
                                y: cgY(458 + 20)), withAttributes: labelAttrs)
        // Y 轴标题旋转 -90°（垂直中心，x=20）。
        let yTitle = tr(.batteryAxisVoltage)
        let yTitleTransform = NSAffineTransform()
        yTitleTransform.translateX(by: 20, yBy: cgY(plotTop + plotH / 2))
        yTitleTransform.rotate(byDegrees: 90)
        yTitleTransform.concat()
        yTitle.draw(at: .zero, withAttributes: labelAttrs)
        NSAffineTransform().concat()

        // 折线 + 6×6 实心圆点。
        lineColor.setStroke()
        let linePath = NSBezierPath()
        linePath.lineWidth = 2
        for (index, sample) in samples.enumerated() {
            let point = NSPoint(x: mapX(xs[index]), y: mapY(Double(sample.vbatMv)))
            if index == 0 {
                linePath.move(to: point)
            } else {
                linePath.line(to: point)
            }
        }
        linePath.stroke()
        lineColor.setFill()
        for (index, sample) in samples.enumerated() {
            let rect = NSRect(x: mapX(xs[index]) - 3, y: mapY(Double(sample.vbatMv)) - 3,
                              width: 6, height: 6)
            NSBezierPath(ovalIn: rect).fill()
        }

        guard let image = context.makeImage() else {
            throw NSError(domain: "BatteryMonitor", code: 2, userInfo: [
                NSLocalizedDescriptionKey: "PNG encode/save failed"
            ])
        }
        let bitmap = NSBitmapImageRep(cgImage: image)
        guard let data = bitmap.representation(using: .png, properties: [:]) else {
            throw NSError(domain: "BatteryMonitor", code: 3, userInfo: [
                NSLocalizedDescriptionKey: "PNG encode/save failed"
            ])
        }
        return data
    }

    // MARK: - 关闭

    @objc private func closeWindow() {
        close()
    }

    /// 红叉/Close 同一路径：杀全部定时器、回调 on_closed；监测中关窗 = 会话
    /// 静默终止（对齐 Windows：无确认、主机不发收尾命令）。
    func windowWillClose(_ notification: Notification) {
        killAllTimers()
        onClosed?()
    }

    deinit {
        killAllTimers()
    }
}
