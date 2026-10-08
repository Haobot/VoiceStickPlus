import Foundation

// N1 切5 第一步：状态出口协议化——VoiceStickCoordinator 直呼 StatusController 69 处
// （17 去重方法），成员类型换本协议后类可脱离 App 模块（AppKit 真耦合仅 4 处已勘明）。
// 协议不能带参数默认值：调用点省略的尾部参数由协议 extension 重载承接（参数个数不同
// 故非重复声明）。数据类型 ConnectedVoiceStickDevice / DeviceFirmwareInfo 随协议入 Core
//（纯数据；DeviceClass 本在 Core）。

/// 配对设备展示条目（原 VoiceStickApp/BleCentral.swift，随状态出口协议下沉）。
public struct ConnectedVoiceStickDevice: Equatable {
    public let name: String
    public let deviceID: String
    /// 输入设备类别：StickS3（VS-XXXX）或小米遥控器 2 Pro（RC-XXXX，ATVV 协议）。
    public let deviceClass: DeviceClass

    public init(name: String, deviceID: String, deviceClass: DeviceClass) {
        self.name = name
        self.deviceID = deviceID
        self.deviceClass = deviceClass
    }
}

/// 固件信息展示行（原 VoiceStickApp/FirmwareManifest.swift，随状态出口协议下沉）。
public struct DeviceFirmwareInfo: Equatable {
    public var hardware: String?
    public var currentVersion: String?
    public var latestVersion: String?
    public var updateAvailable = false
    public var isChecking = false
    public var errorMessage: String?

    public init(hardware: String? = nil, currentVersion: String? = nil,
                latestVersion: String? = nil, updateAvailable: Bool = false,
                isChecking: Bool = false, errorMessage: String? = nil) {
        self.hardware = hardware
        self.currentVersion = currentVersion
        self.latestVersion = latestVersion
        self.updateAvailable = updateAvailable
        self.isChecking = isChecking
        self.errorMessage = errorMessage
    }
}

/// 状态/覆盖层出口（对齐 StatusController 现有 17 方法；App 侧由
/// extension StatusController: VoiceStickStatusSink 满足）。
public protocol VoiceStickStatusSink: AnyObject {
    func setStatus(_ text: String)
    func setPairedDeviceIDs(_ deviceIDs: [String])
    func setConnectedDevices(_ devices: [ConnectedVoiceStickDevice])
    func setFirmwareInfo(_ infoByDeviceID: [String: DeviceFirmwareInfo])
    func setDeviceEncoderPresent(_ deviceID: String, present: Bool)
    func setDeviceBattery(_ deviceID: String, level: Int, charging: Bool, usbPowered: Bool)
    func setAirMouseActive(_ active: Bool, deviceID: String)
    func setHasRecoverableInput(_ hasRecoverableInput: Bool)

    func showListening(deviceID: String?)
    func showPartial(_ text: String, deviceID: String?)
    func showRefining(_ text: String, deviceID: String?)
    func appendPartial(_ text: String, deviceID: String?)
    func showPausedFinal(_ text: String, deviceID: String?)
    func showFinal(_ text: String, deviceID: String?, onHidden: (() -> Void)?)
    func showTimedMessage(_ text: String, duration: TimeInterval, deviceID: String?,
                          onHidden: (() -> Void)?)
    func showError(_ text: String, deviceID: String?, onHidden: (() -> Void)?)
    func hideOverlay(onHidden: (() -> Void)?)
    func hideOverlay(deviceID: String?, onHidden: (() -> Void)?)
}

// 省略尾参的调用形（协议无默认值 → extension 提供参数个数更少的重载转发全参需求）。
public extension VoiceStickStatusSink {
    func showListening() { showListening(deviceID: nil) }
    func showPartial(_ text: String) { showPartial(text, deviceID: nil) }
    func showRefining(_ text: String) { showRefining(text, deviceID: nil) }
    func appendPartial(_ text: String) { appendPartial(text, deviceID: nil) }
    func showPausedFinal(_ text: String) { showPausedFinal(text, deviceID: nil) }
    func showFinal(_ text: String) { showFinal(text, deviceID: nil, onHidden: nil) }
    func showFinal(_ text: String, deviceID: String?) {
        showFinal(text, deviceID: deviceID, onHidden: nil)
    }
    func showError(_ text: String) { showError(text, deviceID: nil, onHidden: nil) }
    func showError(_ text: String, deviceID: String?) {
        showError(text, deviceID: deviceID, onHidden: nil)
    }
    func showTimedMessage(_ text: String, duration: TimeInterval) {
        showTimedMessage(text, duration: duration, deviceID: nil, onHidden: nil)
    }
    func showTimedMessage(_ text: String, duration: TimeInterval, deviceID: String?) {
        showTimedMessage(text, duration: duration, deviceID: deviceID, onHidden: nil)
    }
    func hideOverlay() { hideOverlay(onHidden: nil) }
    func hideOverlay(deviceID: String?) { hideOverlay(deviceID: deviceID, onHidden: nil) }
}
