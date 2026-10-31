import Foundation

// MARK: - 会话音频来源（DebugAudio 文件名前缀 + 侧车标注）

/// 一次语音会话的音频来源。决定 DebugAudio 文件名前缀，供 audio_lab.py
/// 无需侧车也能粗分来源；精确信息以侧车 `source` 字段为准。
///
/// 网关中继（`gatewayRelay`）此前与 Stick 麦共用 `VS-` 前缀、无法区分——
/// 2026-10-31 起依赖固件既有的按键 `source:"xiaomi"` 事件（protocol.md
/// StateEvent）在桌面端标注，零协议/固件改动。
public enum AudioSessionSource: String, Codable, Equatable {
    /// StickS3 自带麦克风（ES8311），文件名前缀 `VS-`。
    case stickMic
    /// 小米遥控器经 Stick 网关中继（外部源时间基修正链路），前缀 `VG-`。
    case gatewayRelay
    /// 小米遥控器直连桌面端（ATVV，桌面端后处理链），前缀 `RC-`。
    case remoteDirect

    public var filenamePrefix: String {
        switch self {
        case .stickMic: return "VS-"
        case .gatewayRelay: return "VG-"
        case .remoteDirect: return "RC-"
        }
    }

    /// 侧车 deviceClass 字段值（由来源反推：直连遥控器 = xiaomi_remote_2pro）。
    public var deviceClassTag: String {
        self == .remoteDirect ? "xiaomi_remote_2pro" : "stick_s3"
    }

    /// 纯函数解析（对齐 ProviderPopupMapping 的可测模式）：
    /// - 设备为小米遥控器（直连 ATVV）→ `remoteDirect`；
    /// - Stick 设备且触发按键带 `source == "xiaomi"`（网关模式遥控器语音键）
    ///   → `gatewayRelay`；
    /// - 其余（物理主键 / 编码器 / 热键经固件 remote_button，均无 source 或
    ///   非 xiaomi）→ `stickMic`。
    public static func resolve(triggerSource: String?, deviceClass: DeviceClass?) -> AudioSessionSource {
        if deviceClass == .xiaomiRemote2Pro { return .remoteDirect }
        if triggerSource == "xiaomi" { return .gatewayRelay }
        return .stickMic
    }
}

// MARK: - BLE 音频帧序号缺口（实测丢帧）

/// 按 `AudioFrame.seq` 累计缺口——把「帧覆盖率推断丢帧」升级为实测。
/// 会话内 seq 单调递增（uint32 不回绕），重复/乱序帧按 0 缺口忽略。
public struct AudioSeqGapTracker: Equatable {
    public private(set) var droppedFrames: Int = 0
    private var lastSeq: UInt32?

    public init() {}

    public mutating func feed(_ seq: UInt32) {
        guard let last = lastSeq else {
            lastSeq = seq
            return
        }
        // 重复/乱序（回拨）帧：忽略且不回拨 last——重传与旧帧不算新缺口。
        guard seq > last else { return }
        lastSeq = seq
        // uint32 溢出防护：正常会话帧数远小于 Int32 上限。
        let delta = Int(seq) - Int(last)
        if delta > 1 { droppedFrames += delta - 1 }
    }

    public mutating func reset() {
        droppedFrames = 0
        lastSeq = nil
    }
}

// MARK: - DebugAudio 会话侧车（<同名 ogg>.json）

/// DebugAudio 会话元数据，与 ogg 同名相邻存放（`X.ogg` ↔ `X.json`）。
/// audio_lab.py 读取它展示传输层指标与现场识别文本；字段全部可缺省，
/// 历史/Windows 文件无侧车时工具按"—"容错。
public struct DebugAudioSidecar: Codable, Equatable {
    public static let schemaVersion = 1

    public var schema: Int = schemaVersion
    /// `AudioSessionSource.rawValue`（"stickMic"/"gatewayRelay"/"remoteDirect"）。
    public var source: String
    /// `AudioSessionSource.deviceClassTag`（"stick_s3"/"xiaomi_remote_2pro"）。
    public var deviceClass: String
    public var sessionID: UInt32?
    /// 非空音频帧数（Audio stream summary 口径）。
    public var frames: Int
    /// 首末帧到达跨度（秒）；span ≫ 说话时长 = BLE 积压。
    public var streamSpanSeconds: Double
    /// 按键录音时长（秒）。
    public var recordingSeconds: Double
    /// seq 缺口实测丢帧数（0 = 无丢帧）。
    public var droppedFrames: Int
    /// 本会话是否开启 RNNoise 增强（true 时存在 `<名>.raw.ogg` 增强前原件）。
    public var noiseSuppression: Bool
    /// 现场识别最终文本（ASR onFinal 后补写；取消/出错会话缺省）。
    public var finalText: String?
    public var savedAt: Date

    public init(
        source: String,
        deviceClass: String,
        sessionID: UInt32? = nil,
        frames: Int,
        streamSpanSeconds: Double,
        recordingSeconds: Double,
        droppedFrames: Int,
        noiseSuppression: Bool,
        finalText: String? = nil,
        savedAt: Date = Date()
    ) {
        self.source = source
        self.deviceClass = deviceClass
        self.sessionID = sessionID
        self.frames = frames
        self.streamSpanSeconds = streamSpanSeconds
        self.recordingSeconds = recordingSeconds
        self.droppedFrames = droppedFrames
        self.noiseSuppression = noiseSuppression
        self.finalText = finalText
        self.savedAt = savedAt
    }

    // MARK: 读写（原子写；缺字段/坏文件返回 nil，不抛）

    public func write(to url: URL) -> Bool {
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        guard let data = try? encoder.encode(self) else { return false }
        do {
            try FileManager.default.createDirectory(
                at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            try data.write(to: url, options: .atomic)
            return true
        } catch {
            return false
        }
    }

    public static func read(from url: URL) -> DebugAudioSidecar? {
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        guard let data = try? Data(contentsOf: url) else { return nil }
        return try? decoder.decode(DebugAudioSidecar.self, from: data)
    }

    /// ASR 完成后补写 finalText（读-改-写；文件不存在即忽略）。
    @discardableResult
    public static func updateFinalText(_ text: String, at url: URL) -> Bool {
        guard var sidecar = read(from: url) else { return false }
        sidecar.finalText = text
        return sidecar.write(to: url)
    }
}
