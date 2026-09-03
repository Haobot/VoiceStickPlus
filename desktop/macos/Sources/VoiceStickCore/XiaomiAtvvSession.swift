import Foundation

/// 会话输出动作（调用方/BLE 层消费）：写 TX 字节 / 合成 StateEvent / 合成 AudioFrame / 错误。
public enum XiaomiAtvvAction {
    case writeTx(Data)
    case stateEvent(StateEvent)
    case audioFrame(AudioFrame)
    case error(String) // "unsupported_codec" / "caps_timeout"
}

public enum XiaomiAtvvSessionState {
    case idle
    case capsRequested
    case ready
    case tapPending // 短击判定中：MIC_OPEN 已应答，等待 300ms 长按阈值或 STOP
    case streaming // button_down 已发出，音频流出中
    case draining // STOP 已收到，150ms 尾包宽限内
    case waitSecondTap // 短击已释放，双击窗内等第二次按下
    case error
}

/// 小米遥控器 ATVV 会话纯状态机（不碰 CoreBluetooth）：输入 Control opcode / Audio 字节 /
/// 注入时钟 nowMs，输出动作列表。两种开麦入径：RC003 发 MIC_OPEN(0x08) 等主机
/// 回 0x0C ACK 后再发 STREAM_START(0x04)；2 Pro 按下直接发 0x04（按下+开流一体帧，
/// 无 0x08、无需 ACK，byte1=interaction、byte2=codec、byte3=会话计数）。按键语义
/// 镜像固件双击设计（Doc/Plan/primary-button-double-click.md）：
/// - hold_to_talk：MIC_OPEN 先缓冲音频不发事件；按住 ≥300ms 确认长按发 button_down
///   （缓冲音频不丢）；<300ms 松开为短击，丢弃缓冲，进入双击窗；窗内第二次按下合成
///   button_double_click（不录音）；窗超时补发 button_click（协调器侧为无害 no-op）。
/// - click_to_talk：MIC_OPEN 立即发 button_down（协调器按 down/up 处理），第一次点击
///   即已开录、松开即停，双击窗超时不再补发 button_click（否则协调器空闲态会把
///   无 duration_ms 的 click 当启动，产生永远收不到音频的幽灵会话）；仅保留窗内
///   第二击合成 button_double_click。
/// - 重开拒绝窗：经过 streaming 的键程 STOP 后 300ms 内忽略 MIC_OPEN（不回 ACK、
///   不开新会话）；双击路径走 waitSecondTap 不受影响。
/// 音频路径：ADPCM 累积切帧 → IMA 解码 → 三点平滑+增益 → Opus（640 采样/帧）→
/// AudioFrame（sessionID 每录音会话自增，与 button_down 一致；flags bit0 首帧 bit1 末帧）。
/// 线程契约：全部公开入口（start/stop/handleControlCommand/handleAudioData/tick）须在
/// 单一线程串行调用（集成层负责 marshal 到主线程），本类无内部锁。
public final class XiaomiAtvvSession {
    public struct Options {
        public var interactionMode: InteractionMode = .holdToTalk
        public var gainDb = 12.0 // ADPCM 解码后增益（±24dB 限幅）
        public var doubleClickWindowMs = 350 // [device.<id>.xiaomi] double_click_ms；
        // 有意小于固件 500ms 窗（更快确认单击），勿对齐

        public init(interactionMode: InteractionMode = .holdToTalk, gainDb: Double = 12.0,
                    doubleClickWindowMs: Int = 350) {
            self.interactionMode = interactionMode
            self.gainDb = gainDb
            self.doubleClickWindowMs = doubleClickWindowMs
        }
    }

    /// 时序常量：hold 阈值对齐固件 DOUBLE_CLICK_HOLD_THRESHOLD_MS；尾包宽限见协议 §3.2。
    public static let holdThresholdMs: Int64 = 300
    public static let audioTailGraceMs: Int64 = 150
    public static let capsTimeoutMs: Int64 = 2000
    /// STOP（长按键程）后拒绝重开会话的窗口时长。
    public static let reopenRejectMs: Int64 = 300
    /// F5 抑制窗：仅「最近 80ms 内有开麦迹象」时吞掉 F5。
    public static let f5SuppressWindowMs: Int64 = 80
    /// tapPending 暂存帧防御性上限（300ms 阈值下正常 ≤8 帧）。
    public static let maxPendingPayloads = 100

    public private(set) var state: XiaomiAtvvSessionState = .idle
    /// 当前/最近录音会话 id（未发过 button_down 时为 0）。
    public private(set) var currentSessionID: UInt32 = 0

    private let options: Options
    private var legacyLayout = false // CAPS 版本 < v1.0，影响 TX 命令格式

    private let decoder = ImaAdpcmDecoder()
    private let accumulator = FrameAccumulator()
    private let postprocessor: PcmPostprocessor
    private let encoder: AudioOpusEncoder
    private let slicer = OpusFrameSlicer()

    private var capsRequestedAt: Int64 = 0
    private var pressStartedAt: Int64 = 0 // 当前按下 MIC_OPEN 时刻
    private var stopReceivedAt: Int64 = 0 // 当前/最近 STOP 时刻
    private var doubleClickDeadline: Int64 = 0
    private var rejectReopenUntil: Int64 = 0 // 长按键程 STOP 后的重开拒绝窗截止时刻
    private var pressSuppressed = false // 当前按下被双击消费：音频丢弃、不发事件
    private var micOpenRemote = false // 遥控器侧 mic 打开未 STOP，断开需 MIC_CLOSE
    private var streamActive = false // 已收到 0x04，音频有效
    private var remoteSessionID: UInt8 = 0 // 0x04 bytes[3]（可选），MIC_CLOSE 透传

    private var nextSessionID: UInt32 = 1 // 每次发出 button_down 时取用并自增
    private var nextSeq: UInt32 = 1
    private var sessionFrameStarted = false // 首帧 start flag 是否已打
    /// tapPending 期间已编码未确认归属会话的 Opus 帧（确认长按后补 session/seq 发出）。
    private var pendingPayloads: [Data] = []

    /// AudioOpusEncoder 创建失败时抛错。
    public init(options: Options = Options()) throws {
        self.options = options
        postprocessor = PcmPostprocessor(gainDb: options.gainDb)
        encoder = try AudioOpusEncoder()
    }

    /// F5 抑制判定（macOS 侧由事件钩子消费）：小米语音键按下时遥控器固件会向 OS
    /// 多发一个 F5 键；仅「最近 80ms 内有开麦迹象」时吞掉。
    /// lastMicOpenMs 为 nil 或 ≤0 表示从未开麦（不吞）。
    public static func shouldSuppressF5(nowMs: Int64, lastMicOpenMs: Int64?, enabled: Bool) -> Bool {
        guard enabled, let lastMicOpenMs, lastMicOpenMs > 0 else { return false }
        let ageMs = nowMs - lastMicOpenMs
        return ageMs >= 0 && ageMs <= f5SuppressWindowMs
    }

    /// 连接建立后调用：进入 capsRequested 并产出写 TX GET_CAPS。仅 idle 可启动。
    public func start(nowMs: Int64) -> [XiaomiAtvvAction] {
        guard state == .idle else { return [] }
        capsRequestedAt = nowMs
        state = .capsRequested
        return [.writeTx(XiaomiAtvvProtocol.getCapsCommand())]
    }

    /// 断开前调用：若遥控器侧 mic 仍打开则尽力发 MIC_CLOSE，并复位到 idle。
    public func stop(nowMs _: Int64) -> [XiaomiAtvvAction] {
        var actions: [XiaomiAtvvAction] = []
        if micOpenRemote {
            actions.append(.writeTx(XiaomiAtvvProtocol.micCloseCommand(
                legacyLayout: legacyLayout, sessionID: remoteSessionID)))
        }
        // 复位到 idle：重连后需重新 start 握手；session 计数保持递增不复用。
        state = .idle
        micOpenRemote = false
        streamActive = false
        pressSuppressed = false
        legacyLayout = false
        rejectReopenUntil = 0
        capsRequestedAt = 0
        pressStartedAt = 0
        stopReceivedAt = 0
        doubleClickDeadline = 0
        decoder.reset()
        encoder.reset()
        accumulator.reset()
        slicer.reset()
        pendingPayloads.removeAll()
        currentSessionID = 0
        nextSeq = 1
        sessionFrameStarted = false
        return actions
    }

    public func handleControlCommand(_ data: Data, nowMs: Int64) -> [XiaomiAtvvAction] {
        var actions: [XiaomiAtvvAction] = []
        guard state != .error, !data.isEmpty else { return actions }
        // Data 切片 startIndex 不归零（下方 data[0]/[2]/[3]... 是绝对下标，切片会
        // 越界崩溃）：入口拷贝归一化（实测 Data(data) 归零 startIndex）。
        let data = Data(data)

        switch data[0] {
        case XiaomiAtvvProtocol.controlCaps:
            guard state == .capsRequested || state == .ready else { break }
            guard let caps = XiaomiAtvvProtocol.parseCaps(data) else { break } // 坏包忽略，由 CAPS 超时兜底
            guard caps.supports16kHz else {
                state = .error
                actions.append(.error("unsupported_codec"))
                break
            }
            legacyLayout = !caps.isV1OrLater
            accumulator.setFrameBytes(caps.frameBytes)
            state = .ready

        case XiaomiAtvvProtocol.controlMicOpen:
            handlePressFrame(&actions, nowMs: nowMs, sendAck: true)

        case XiaomiAtvvProtocol.controlStreamStart:
            if state == .tapPending || state == .streaming {
                // RC003 会话内的流开始/重启标记：硬重置（编码器从 0/0 重启但可能
                // 不发 SYNC），否则第二次按键 DC 饱和。
                hardResetStream()
                if data.count >= 4 { remoteSessionID = data[3] }
                break
            }
            if state == .draining || state == .ready || state == .waitSecondTap {
                // 2 Pro：0x04 即「按下+开流」一体帧（无 0x08、无需 0x0C ACK）。
                // byte2=codec 存在时必须为 16kHz（对齐 MiVibe 对 8kHz 的拒绝）。
                if data.count >= 3, data[2] != XiaomiAtvvProtocol.codecMask16kHz {
                    actions.append(.error("unsupported_codec"))
                    break
                }
                if handlePressFrame(&actions, nowMs: nowMs, sendAck: false) {
                    hardResetStream()
                    if data.count >= 4 { remoteSessionID = data[3] }
                }
                break
            }
            // idle/capsRequested：握手未完成，忽略

        case XiaomiAtvvProtocol.controlAudioSync:
            guard data.count >= 7 else { break }
            guard state == .tapPending || state == .streaming || state == .draining else { break }
            let predictor = Int16(bitPattern: UInt16(data[4]) << 8 | UInt16(data[5]))
            decoder.reset(predictor: predictor, stepIndex: Int(data[6]))
            accumulator.reset()
            slicer.reset()
            pendingPayloads.removeAll()

        case XiaomiAtvvProtocol.controlStop:
            micOpenRemote = false
            // 注意不清 streamActive：Audio 与 Control 是两条独立特征，
            // STOP 后 150ms 宽限内的音频尾包仍须接收（finalizeStream 才清）。
            stopReceivedAt = nowMs
            if state == .tapPending {
                discardPressBuffer()
                if pressSuppressed {
                    // 被双击消费的第二次按下：松开不再发事件。
                    pressSuppressed = false
                    state = .ready
                } else {
                    // 短击：进入双击窗，窗超时由 tick 发 button_click。
                    doubleClickDeadline = nowMs + Int64(options.doubleClickWindowMs)
                    state = .waitSecondTap
                }
            } else if state == .streaming {
                actions.append(.stateEvent(makePrimaryButtonEvent("button_up", sessionID: nil)))
                // 音频尾包可能晚于 STOP 到达：留 150ms 宽限。
                state = .draining
                // 规格：STOP 后 300ms 内拒绝重开会话（防遥控器抖动/急速重开）。
                // 仅长按键程（经过 streaming）武装；短击走 waitSecondTap 不设窗。
                rejectReopenUntil = nowMs + Self.reopenRejectMs
            }

        default:
            break
        }
        return actions
    }

    public func handleAudioData(_ data: Data, nowMs: Int64) -> [XiaomiAtvvAction] {
        var actions: [XiaomiAtvvAction] = []
        guard state == .tapPending || state == .streaming || state == .draining else { return actions }
        guard streamActive, !pressSuppressed else { return actions }
        // 超宽限的尾包丢弃。
        if state == .draining, nowMs - stopReceivedAt > Self.audioTailGraceMs { return actions }
        for adpcmFrame in accumulator.append(data) {
            let pcm = decoder.decode(adpcmFrame)
            for frame in slicer.append(pcm) {
                emitPcmFrame(&actions, pcm: frame)
            }
        }
        return actions
    }

    /// 周期调用：处理长按阈值确认、尾包宽限到期、双击窗到期、CAPS 超时。
    public func tick(nowMs: Int64) -> [XiaomiAtvvAction] {
        var actions: [XiaomiAtvvAction] = []
        switch state {
        case .capsRequested:
            if nowMs - capsRequestedAt >= Self.capsTimeoutMs {
                state = .error
                actions.append(.error("caps_timeout"))
            }
        case .tapPending:
            // 按住 ≥300ms 确认长按；被双击消费的按下不确认。
            if !pressSuppressed, nowMs - pressStartedAt >= Self.holdThresholdMs {
                confirmLongPress(&actions)
            }
        case .draining:
            if nowMs - stopReceivedAt >= Self.audioTailGraceMs {
                finalizeStream(&actions)
            }
        case .waitSecondTap:
            if nowMs >= doubleClickDeadline {
                // 双击窗超时无第二次按下。hold_to_talk：短击未发过事件，补发
                // button_click（协调器侧为无害 no-op）；click_to_talk：第一次点击
                // 已由 down/up 完整表达并开录，补发会让协调器空闲态启动幽灵会话，
                // 故只回 ready 不发事件。
                if options.interactionMode == .holdToTalk {
                    actions.append(.stateEvent(makePrimaryButtonEvent("button_click", sessionID: nil)))
                }
                state = .ready
            }
        default:
            break
        }
        return actions
    }

    /// MIC_OPEN(0x08)/STREAM_START(0x04) 合一的「按下」入口：双击窗判定、STOP 后
    /// 重开拒绝窗、draining 收尾后开新按下。返回 true 表示已接受按下（调 beginPress）。
    @discardableResult
    private func handlePressFrame(_ actions: inout [XiaomiAtvvAction], nowMs: Int64,
                                  sendAck: Bool) -> Bool {
        if state == .waitSecondTap {
            if nowMs <= doubleClickDeadline {
                // 窗内第二次按下：合成 button_double_click（桌面端注入 Enter），
                // 本次按下被消费：仍按协议应答，但不录音、不发 down/up。
                actions.append(.stateEvent(makePrimaryButtonEvent("button_double_click", sessionID: nil)))
                beginPress(&actions, nowMs: nowMs, suppressed: true, sendAck: sendAck)
            } else {
                // tick 未跑窗口已过。hold_to_talk 的短击尚未发过任何事件，补发
                // button_click 表达本次单击；click_to_talk 的第一次点击已由
                // down/up 完整表达并开录，补发会让协调器空闲态启动幽灵会话。
                if options.interactionMode == .holdToTalk {
                    actions.append(.stateEvent(makePrimaryButtonEvent("button_click", sessionID: nil)))
                }
                state = .ready
                beginPress(&actions, nowMs: nowMs, suppressed: false, sendAck: sendAck)
            }
            return true
        }
        // STOP 后 300ms 重开拒绝窗（draining 与窗内已回 ready 均生效）：
        // 不回 0x0C ACK、不开新会话。双击路径走 waitSecondTap 分支，不受影响。
        if (state == .draining || state == .ready), nowMs < rejectReopenUntil {
            return false
        }
        if state == .draining {
            // 拒绝窗外重开：先收尾当前会话再开新按下。
            finalizeStream(&actions)
            beginPress(&actions, nowMs: nowMs, suppressed: false, sendAck: sendAck)
            return true
        }
        guard state == .ready else { return false }
        beginPress(&actions, nowMs: nowMs, suppressed: false, sendAck: sendAck)
        return true
    }

    /// 0x04 到达时的流硬重置：RC003 每次会话编码器从 0/0 重启但可能不发 SYNC，
    /// 一律重置，否则第二次按键 DC 饱和。
    private func hardResetStream() {
        decoder.reset()
        accumulator.reset()
        slicer.reset()
        pendingPayloads.removeAll()
        streamActive = true
    }

    /// 开麦应答 + 按键按下登记（handlePressFrame 各分支共用）。sendAck=false
    ///（2 Pro 的 0x04 直开，协议无 0x0C ACK）时跳过写 TX。
    private func beginPress(_ actions: inout [XiaomiAtvvAction], nowMs: Int64,
                            suppressed: Bool, sendAck: Bool = true) {
        if sendAck {
            actions.append(.writeTx(XiaomiAtvvProtocol.micOpenAckCommand(legacyLayout: legacyLayout)))
        }
        micOpenRemote = true
        streamActive = false
        pressStartedAt = nowMs
        pressSuppressed = suppressed
        pendingPayloads.removeAll()
        if !suppressed {
            // 对齐固件（audio_pipeline.c：每次录音会话开始 OPUS_RESET_STATE）：
            // 新按下即重置 Opus 编码器，杜绝跨会话状态泄漏。hold_to_talk 的
            // tapPending 暂存帧与 click_to_talk 的立即流都从这里开始编码，
            // confirmLongPress 不再重复 reset（避免暂存帧与确认后流帧之间
            // 编码器状态被人为切断）。
            encoder.reset()
        }
        if !suppressed, options.interactionMode == .clickToTalk {
            // click_to_talk：MIC_OPEN 立即发 button_down（协调器按 down/up 处理）。
            currentSessionID = nextSessionID
            nextSessionID += 1
            nextSeq = 1
            sessionFrameStarted = false
            actions.append(.stateEvent(makePrimaryButtonEvent("button_down", sessionID: currentSessionID)))
            state = .streaming
        } else {
            state = .tapPending
        }
    }

    /// 640 采样 PCM 帧 → 后处理 → Opus → AudioFrame 动作（tapPending 中改为暂存）。
    private func emitPcmFrame(_ actions: inout [XiaomiAtvvAction], pcm: [Int16]) {
        let processed = postprocessor.process(pcm)
        guard let encoded = encoder.encode(processed) else { return }
        if state == .tapPending {
            // 长按未确认：暂存（确认后补 session/seq 发出，不丢前 300ms 语音）。
            // 防御性上限：tick 失能时避免无界增长（300ms 阈值下正常 ≤8 帧）。
            if pendingPayloads.count < Self.maxPendingPayloads {
                pendingPayloads.append(encoded)
            }
            return
        }
        let frame = AudioFrame(sessionID: currentSessionID, seq: nextSeq,
                               flags: sessionFrameStarted ? 0x00 : 0x01, payload: encoded)
        nextSeq += 1
        sessionFrameStarted = true
        actions.append(.audioFrame(frame))
    }

    /// 长按确认：发 button_down 并放出暂存帧，进入 streaming。
    private func confirmLongPress(_ actions: inout [XiaomiAtvvAction]) {
        // 编码器状态已在 beginPress 重置，此处不再 reset（保持暂存帧与后续流帧连续）。
        currentSessionID = nextSessionID
        nextSessionID += 1
        nextSeq = 1
        sessionFrameStarted = false
        actions.append(.stateEvent(makePrimaryButtonEvent("button_down", sessionID: currentSessionID)))
        state = .streaming
        // 暂存帧补 session/seq 后按序流出（首帧打 start flag）。
        for payload in pendingPayloads {
            let frame = AudioFrame(sessionID: currentSessionID, seq: nextSeq,
                                   flags: sessionFrameStarted ? 0x00 : 0x01, payload: payload)
            nextSeq += 1
            sessionFrameStarted = true
            actions.append(.audioFrame(frame))
        }
        pendingPayloads.removeAll()
    }

    /// STOP 后的收尾：宽限到期/被新按下打断时补末帧（end flag）并回到 ready/waitSecondTap。
    private func finalizeStream(_ actions: inout [XiaomiAtvvAction]) {
        // 余量补零出末帧（end flag）；无有效音频帧的会话不产生任何 AudioFrame。
        var remainder = slicer.takeRemainder()
        if !remainder.isEmpty {
            remainder.append(contentsOf: repeatElement(0, count: AudioOpusEncoder.frameSamples - remainder.count))
            let processed = postprocessor.process(remainder)
            if let encoded = encoder.encode(processed) {
                var flags: UInt8 = sessionFrameStarted ? 0x00 : 0x01
                flags |= 0x02
                let frame = AudioFrame(sessionID: currentSessionID, seq: nextSeq,
                                       flags: flags, payload: encoded)
                nextSeq += 1
                sessionFrameStarted = true
                actions.append(.audioFrame(frame))
            }
        } else if sessionFrameStarted {
            // 无余量但本会话已有帧：补空 payload end 帧让协调器收尾（既有空 END 路径）。
            let frame = AudioFrame(sessionID: currentSessionID, seq: nextSeq,
                                   flags: 0x02, payload: Data())
            nextSeq += 1
            actions.append(.audioFrame(frame))
        }
        accumulator.reset()
        pendingPayloads.removeAll()
        streamActive = false
        // click_to_talk 的短按（按下即开录、松开即停）武装双击窗；长按直接回 ready。
        if options.interactionMode == .clickToTalk,
           stopReceivedAt - pressStartedAt < Self.holdThresholdMs {
            doubleClickDeadline = stopReceivedAt + Int64(options.doubleClickWindowMs)
            state = .waitSecondTap
        } else {
            state = .ready
        }
    }

    /// 丢弃当前按下的音频缓冲（短击/被双击消费的按下）。
    private func discardPressBuffer() {
        pendingPayloads.removeAll()
        slicer.reset()
        accumulator.reset()
        decoder.reset() // 防御性复位；下个会话 0x04 还会硬重置
        streamActive = false
    }
}

/// 合成物理主键 StateEvent（macOS StateEvent 无 source 字段，Windows 侧同样留空）。
private func makePrimaryButtonEvent(_ event: String, sessionID: UInt32?) -> StateEvent {
    StateEvent(event: event, button: "primary", sessionID: sessionID,
               durationMs: nil, hardware: nil, firmwareVersion: nil,
               buttons: nil, uiStates: nil)
}
