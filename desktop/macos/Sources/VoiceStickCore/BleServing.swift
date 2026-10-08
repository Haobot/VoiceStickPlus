import Foundation

// N1 切5 第二步：BLE 出口协议化——VoiceStickCoordinator 对 BleCentral 的 24 面依赖
//（8 属性含 6 个 on* 回调与 2 个 provider + 16 方法）换本协议后，类仅剩 4 处 AppKit
// 与 public 化两事即可迁 Core。具体类 BleCentral 留 App，由 extension 合规；构造外置
// 注入（AppDelegate 持具体实例）。FirmwareUpdateProgress 随协议入 Core（纯数据）。
// 省略尾参（to: / text:）的调用形由协议 extension 重载承接。

/// 固件升级进度（原 VoiceStickApp/BleCentral.swift，随 BLE 协议下沉）。
public struct FirmwareUpdateProgress: Equatable {
    public let writtenBytes: Int
    public let totalBytes: Int
    public let isDeviceConfirmed: Bool

    public var fraction: Double {
        guard totalBytes > 0 else { return 0 }
        return Double(writtenBytes) / Double(totalBytes)
    }

    public init(writtenBytes: Int, totalBytes: Int, isDeviceConfirmed: Bool) {
        self.writtenBytes = writtenBytes
        self.totalBytes = totalBytes
        self.isDeviceConfirmed = isDeviceConfirmed
    }
}

/// BLE 服务出口（对齐 BleCentral 现有 24 面；事件类型均在 Core）。
public protocol VoiceStickBleServing: AnyObject {
    var onConnectionChange: (([ConnectedVoiceStickDevice]) -> Void)? { get set }
    var onStateEvent: ((UUID, StateEvent) -> Void)? { get set }
    var onAudioFrame: ((UUID, AudioFrame) -> Void)? { get set }
    var onMotionFrame: ((UUID, MotionFrame) -> Void)? { get set }
    var onPowerLogFragment: ((UUID, PowerLogFragment) -> Void)? { get set }
    var onPowerMgmtEvent: ((UUID, PowerMgmtEvent) -> Void)? { get set }
    var pairedDevicesProvider: (() -> [PairedDeviceEntry])? { get set }
    var xiaomiOptionsResolver: ((String) -> XiaomiAtvvSession.Options)? { get set }
    var micOpenAnchor: XiaomiMicOpenAnchor { get }

    func start()
    func updatePairedDeviceIDs(_ deviceIDs: [String])
    func connectedStickDeviceIDs() -> [String]
    func deviceClass(for peripheralID: UUID) -> DeviceClass
    func deviceID(for peripheralID: UUID) -> String?
    func peripheralID(forDeviceID deviceID: String) -> UUID?
    func isConnected(_ peripheralID: UUID) -> Bool
    func isConnected(deviceID: String) -> Bool
    func sendInteractionMode(_ mode: InteractionMode, to peripheralID: UUID?)
    func sendPowerLogCommand(_ data: Data, to deviceID: String)
    func sendRemoteButton(action: String, deviceID: String, requestID: UInt32)
    func sendShowIMUDebug(_ enabled: Bool, to peripheralID: UUID?)
    func sendStickControlPayload(_ data: Data, label: String, deviceID: String)
    func sendUIState(_ state: String, text: String, to peripheralID: UUID?)
    func cancelFirmwareUpdate()
    func updateFirmware(image: Data, for deviceID: String,
                        progress: @escaping (FirmwareUpdateProgress) -> Void,
                        completion: @escaping (Result<Void, Error>) -> Void)
}

// 省略尾参的调用形（协议无默认值 → extension 提供参数更少的重载转发全参需求）。
public extension VoiceStickBleServing {
    func sendInteractionMode(_ mode: InteractionMode) {
        sendInteractionMode(mode, to: nil)
    }
    func sendShowIMUDebug(_ enabled: Bool) {
        sendShowIMUDebug(enabled, to: nil)
    }
    func sendUIState(_ state: String) {
        sendUIState(state, text: "", to: nil)
    }
    func sendUIState(_ state: String, to peripheralID: UUID?) {
        sendUIState(state, text: "", to: peripheralID)
    }
}
