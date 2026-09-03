import AppKit
import CoreBluetooth
import Foundation
import VoiceStickCore

struct ConnectedVoiceStickDevice {
    let name: String
    let deviceID: String
    /// 输入设备类别：StickS3（VS-XXXX）或小米遥控器 2 Pro（RC-XXXX，ATVV 协议）。
    let deviceClass: DeviceClass
}

struct FirmwareUpdateProgress {
    let writtenBytes: Int
    let totalBytes: Int
    let isDeviceConfirmed: Bool

    var fraction: Double {
        guard totalBytes > 0 else { return 0 }
        return Double(writtenBytes) / Double(totalBytes)
    }
}

final class BleCentral: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    enum FirmwareUpdateError: LocalizedError {
        case noConnectedDevice
        case otaCharacteristicUnavailable
        case imageTooLarge
        case transferAlreadyActive
        case firmwareUpdateCancelled
        case peripheralWriteFailed(String)
        case deviceError(String)

        var errorDescription: String? {
            switch self {
            case .noConnectedDevice:
                return "No VoiceStick is connected."
            case .otaCharacteristicUnavailable:
                return "The connected firmware does not expose BLE OTA."
            case .imageTooLarge:
                return "Firmware image is larger than the OTA partition."
            case .transferAlreadyActive:
                return "A firmware update is already running."
            case .firmwareUpdateCancelled:
                return "Firmware update cancelled."
            case .peripheralWriteFailed(let message):
                return "BLE write failed: \(message)"
            case .deviceError(let code):
                return "Device rejected OTA: \(code)"
            }
        }
    }

    private struct FirmwareUpdateSession {
        let peripheralID: UUID
        let transferID: UInt32
        let image: Data
        let chunkSize: Int
        var began = false
        var offset = 0
        var lastQueuedProgressOffset = 0
        var ended = false
        let progress: (FirmwareUpdateProgress) -> Void
        let completion: (Result<Void, Error>) -> Void
    }

    /// 小米遥控器（ATVV）per-peripheral 连接上下文：三特征句柄 + 会话状态机。
    /// 对齐 Windows DeviceSession 的 xiaomi_* 字段组。
    private struct XiaomiPeripheralContext {
        var txCharacteristic: CBCharacteristic?
        var audioCharacteristic: CBCharacteristic?
        var controlCharacteristic: CBCharacteristic?
        /// 标准 Battery Service 0x2A19 特征（可选；发现失败只记日志不阻断连接，
        /// 对齐 Windows SetupXiaomiBatteryAsync 语义）。
        var batteryCharacteristic: CBCharacteristic?
        /// 会话建立后发起 Battery Service 发现的一次性标志（防 didDiscoverServices 重复推进）。
        var batteryDiscoveryStarted = false
        var session: XiaomiAtvvSession?
        /// ATVV 订阅链兜底定时器（发起 Control 订阅时武装，链完成/断开/清理取消）。
        var subscribeTimeoutTimer: Timer?
    }

    /// ATVV 订阅链一次性兜底超时（对齐 Windows kSubscribeTimeout 防御；
    /// Windows 为 2.5s，macOS 取 5s 宽值，只兜回调丢失的永久挂起）。
    private static let atvvSubscribeTimeout: TimeInterval = 5.0

    private var pairedDeviceIDs: Set<String>
    private var central: CBCentralManager!
    private var peripherals: [UUID: CBPeripheral] = [:]
    private var discoveredDevices: [UUID: ConnectedVoiceStickDevice] = [:]
    private var connectedDevices: [UUID: ConnectedVoiceStickDevice] = [:]
    private var controlCharacteristics: [UUID: CBCharacteristic] = [:]
    private var otaCharacteristics: [UUID: CBCharacteristic] = [:]
    private var deviceClasses: [UUID: DeviceClass] = [:]
    private var xiaomiContexts: [UUID: XiaomiPeripheralContext] = [:]
    private var xiaomiTickTimer: Timer?
    private var firmwareUpdateSession: FirmwareUpdateSession?
    private var interactionMode: InteractionMode = .holdToTalk
    private var showIMUDebug = false
    private var isWorkspaceSleeping = false

    var onConnectionChange: (([ConnectedVoiceStickDevice]) -> Void)?
    var onAudioFrame: ((UUID, AudioFrame) -> Void)?
    var onStateEvent: ((UUID, StateEvent) -> Void)?
    /// power_log 分片帧回调（state_tx 上行，无 "event" 键；电量监测窗口消费）。
    var onPowerLogFragment: ((UUID, PowerLogFragment) -> Void)?
    /// power_mgmt 事件回调（固件连接时主动推送 / usb_auto_off set 后回推确认）。
    var onPowerMgmtEvent: ((UUID, PowerMgmtEvent) -> Void)?

    /// 标准 Battery Service（小米遥控器电量；对齐 Windows kBatteryServiceUuid/kBatteryLevelUuid）。
    private static let batteryServiceUUID = "180F"
    private static let batteryLevelUUID = "2A19"

    /// 配对设备条目表访问（AppConfig.pairedDevices 注入，同款 pairedDeviceIDs 注入
    /// 方式）：ATVV 通道按 peripheral UUID 反查 RC deviceID、retrievePeripherals
    /// 盲区补偿。nil 时 RC 仅支持 RC-XXXX 名称通道。
    var pairedDevicesProvider: (() -> [PairedDeviceEntry])?
    /// 按 RC deviceID 解析 ATVV 会话参数（gain/doubleClick/interactionMode）；
    /// nil 用 XiaomiAtvvSession.Options 默认值。
    var xiaomiOptionsResolver: ((String) -> XiaomiAtvvSession.Options)?
    /// F5 抑制锚点（AppDelegate 事件钩子读取；写入在 CoreBluetooth 回调内）。
    public let micOpenAnchor = XiaomiMicOpenAnchor()

    init(pairedDeviceIDs: [String]) {
        self.pairedDeviceIDs = Set(pairedDeviceIDs)
        super.init()
    }

    deinit {
        xiaomiTickTimer?.invalidate()
        xiaomiContexts.keys.forEach { cancelXiaomiSubscribeTimeout(for: $0) }
        NSWorkspace.shared.notificationCenter.removeObserver(self)
    }

    /// 配对条目 hardware 段标识（对齐 Windows kHardwareXiaomiRemote2Pro）。
    private static let hardwareXiaomiRemote2Pro = "xiaomi_remote_2_pro"

    /// 单调毫秒时钟（CLOCK_MONOTONIC 语义）：ATVV 会话时序与 F5 锚点共用，
    /// 与 XiaomiAtvvSession 单测注入的 nowMs 同语义（任意递增基准）。
    static func nowMs() -> Int64 {
        Int64(ProcessInfo.processInfo.systemUptime * 1000)
    }

    func start() {
        central = CBCentralManager(delegate: self, queue: .main)
        NSWorkspace.shared.notificationCenter.addObserver(
            self,
            selector: #selector(workspaceWillSleep),
            name: NSWorkspace.willSleepNotification,
            object: nil
        )
        NSWorkspace.shared.notificationCenter.addObserver(
            self,
            selector: #selector(workspaceDidWake),
            name: NSWorkspace.didWakeNotification,
            object: nil
        )
    }

    func updatePairedDeviceIDs(_ deviceIDs: [String]) {
        pairedDeviceIDs = Set(deviceIDs)
        for peripheral in peripherals.values {
            stopXiaomiSessionBestEffort(for: peripheral)
            central.cancelPeripheralConnection(peripheral)
        }
        peripherals.removeAll()
        discoveredDevices.removeAll()
        connectedDevices.removeAll()
        controlCharacteristics.removeAll()
        otaCharacteristics.removeAll()
        deviceClasses.removeAll()
        xiaomiContexts.keys.forEach { cancelXiaomiSubscribeTimeout(for: $0) }
        xiaomiContexts.removeAll()
        syncXiaomiTickTimer()
        failFirmwareUpdate(FirmwareUpdateError.noConnectedDevice)
        onConnectionChange?([])
        scanIfReady()
        // 配对补偿：RC 广播可能不含 ATVV service UUID，过滤扫描永远看不到；
        // 按配对条目存的 peripheral UUID 直接取回重连（同 restoreConnectedPeripherals
        // 的盲区补偿）。
        if let central, central.state == .poweredOn, !isWorkspaceSleeping {
            reconnectPairedXiaomiPeripherals(central)
        }
    }

    func sendUIState(_ state: String, text: String = "", to peripheralID: UUID? = nil) {
        let data = BleProtocol.uiStatePayload(state: state, text: text)
        if let peripheralID {
            if let characteristic = controlCharacteristics[peripheralID] {
                if let peripheral = peripherals[peripheralID] {
                    let deviceID = connectedDevices[peripheralID]?.deviceID ?? "unknown"
                    NSLog("BLE send ui_state state=\(state) dev=VS-\(deviceID) text_len=\(text.count)")
                    peripheral.writeValue(data, for: characteristic, type: .withoutResponse)
                } else {
                    NSLog("BLE send ui_state skipped missing peripheral state=\(state) id=\(peripheralID) text_len=\(text.count)")
                }
            } else {
                NSLog("BLE send ui_state skipped missing characteristic state=\(state) id=\(peripheralID) text_len=\(text.count)")
            }
            return
        }

        NSLog("BLE send ui_state broadcast state=\(state) targets=\(controlCharacteristics.count) text_len=\(text.count)")
        for (id, characteristic) in controlCharacteristics {
            if let peripheral = peripherals[id] {
                let deviceID = connectedDevices[id]?.deviceID ?? "unknown"
                NSLog("BLE send ui_state state=\(state) dev=VS-\(deviceID) text_len=\(text.count)")
                peripheral.writeValue(data, for: characteristic, type: .withoutResponse)
            } else {
                NSLog("BLE send ui_state skipped missing peripheral state=\(state) id=\(id) text_len=\(text.count)")
            }
        }
    }

    func sendInteractionMode(_ mode: InteractionMode, to peripheralID: UUID? = nil) {
        interactionMode = mode
        let data = BleProtocol.interactionModePayload(mode)
        if let peripheralID {
            if let characteristic = controlCharacteristics[peripheralID] {
                peripherals[peripheralID]?.writeValue(data, for: characteristic, type: .withoutResponse)
            }
            return
        }

        for (id, characteristic) in controlCharacteristics {
            peripherals[id]?.writeValue(data, for: characteristic, type: .withoutResponse)
        }
    }

    /// IMU 调试开关下发（对齐 Windows BleCentral::SendShowImuDebug）：记忆当前值，
    /// 新连接控制特征就绪时随 interaction_mode 一起回放。
    func sendShowIMUDebug(_ enabled: Bool, to peripheralID: UUID? = nil) {
        showIMUDebug = enabled
        let data = BleProtocol.showIMUDebugPayload(enabled: enabled)
        if let peripheralID {
            if let characteristic = controlCharacteristics[peripheralID] {
                peripherals[peripheralID]?.writeValue(data, for: characteristic, type: .withoutResponse)
            }
            return
        }

        for (id, characteristic) in controlCharacteristics {
            peripherals[id]?.writeValue(data, for: characteristic, type: .withoutResponse)
        }
    }

    func updateFirmware(image: Data, for deviceID: String,
                        progress: @escaping (FirmwareUpdateProgress) -> Void,
                        completion: @escaping (Result<Void, Error>) -> Void) {
        guard firmwareUpdateSession == nil else {
            completion(.failure(FirmwareUpdateError.transferAlreadyActive))
            return
        }
        guard let peripheralID = connectedDevices.first(where: { $0.value.deviceID == deviceID })?.key,
              let peripheral = peripherals[peripheralID] else {
            completion(.failure(FirmwareUpdateError.noConnectedDevice))
            return
        }
        // 小米遥控器没有 VoiceStick 固件 OTA 概念（协议一期不做），直接拒绝。
        guard (deviceClasses[peripheralID] ?? .stickS3) == .stickS3 else {
            completion(.failure(FirmwareUpdateError.otaCharacteristicUnavailable))
            return
        }
        guard otaCharacteristics[peripheralID] != nil else {
            completion(.failure(FirmwareUpdateError.otaCharacteristicUnavailable))
            return
        }
        guard image.count <= 3 * 1024 * 1024 else {
            completion(.failure(FirmwareUpdateError.imageTooLarge))
            return
        }

        let maxWrite = peripheral.maximumWriteValueLength(for: .withoutResponse)
        let chunkSize = max(20, min(maxWrite - 12, 244))
        firmwareUpdateSession = FirmwareUpdateSession(
            peripheralID: peripheralID,
            transferID: UInt32.random(in: 1...UInt32.max),
            image: image,
            chunkSize: chunkSize,
            progress: progress,
            completion: completion
        )
        progress(FirmwareUpdateProgress(
            writtenBytes: 0,
            totalBytes: image.count,
            isDeviceConfirmed: true
        ))
        sendNextFirmwareUpdateFrame()
    }

    func cancelFirmwareUpdate() {
        failFirmwareUpdate(FirmwareUpdateError.firmwareUpdateCancelled)
    }

    func deviceID(for peripheralID: UUID) -> String? {
        connectedDevices[peripheralID]?.deviceID ?? discoveredDevices[peripheralID]?.deviceID
    }

    /// 外设的设备类别（未发现过时默认 stickS3）。
    func deviceClass(for peripheralID: UUID) -> DeviceClass {
        deviceClasses[peripheralID] ?? .stickS3
    }

    func isConnected(_ peripheralID: UUID) -> Bool {
        connectedDevices[peripheralID] != nil
    }

    func isConnected(deviceID: String) -> Bool {
        connectedDevices.values.contains { $0.deviceID == deviceID }
    }

    /// 已连接 StickS3 设备 ID 列表（全局热键 remote_button 目标解析用；
    /// 小米遥控器无 remote_button 概念，不参与）。
    func connectedStickDeviceIDs() -> [String] {
        connectedDevices.values
            .filter { $0.deviceClass == .stickS3 }
            .map(\.deviceID)
            .sorted()
    }

    /// 发送 power_log 命令帧（对齐 Windows BleCentralWin::SendPowerLogCommand）：
    /// 仅 StickS3；目标不在线/控制特征未就绪时静默丢弃并记日志。
    func sendPowerLogCommand(_ data: Data, to deviceID: String) {
        sendStickControlPayload(data, label: "power_log_cmd", deviceID: deviceID)
    }

    /// 发送 remote_button 控制帧（对齐 Windows BleCentralWin::SendRemoteButton）。
    /// action: "down"/"up"；仅当目标设备在线且控制特征就绪时发送。
    func sendRemoteButton(action: String, deviceID: String, requestID: UInt32) {
        guard let peripheralID = connectedDevices.first(where: {
            $0.value.deviceID == deviceID && $0.value.deviceClass == .stickS3
        })?.key,
              let characteristic = controlCharacteristics[peripheralID],
              let peripheral = peripherals[peripheralID] else {
            NSLog("BLE send remote_button_\(action) skipped dev=VS-\(deviceID) (not connected)")
            return
        }
        let data = BleProtocol.remoteButtonPayload(
            action: action, button: "primary", source: "global_hotkey", requestID: requestID
        )
        NSLog("BLE send remote_button_\(action) dev=VS-\(deviceID) request_id=\(requestID)")
        peripheral.writeValue(data, for: characteristic, type: .withoutResponse)
    }

    /// 设备交互/编码器设置下发（对齐 Windows BleCentralWin::Send{TapEnabled,TapSensitivity,
    /// ImuWakeSensitivity,EncoderLedColor,EncoderRecordingGate}）：按 deviceID 寻址单播，
    /// 仅 StickS3（小米遥控器无 IMU/敲击/编码器硬件，按类门控兜底）。
    func sendStickControlPayload(_ data: Data, label: String, deviceID: String) {
        guard let peripheralID = connectedDevices.first(where: {
            $0.value.deviceID == deviceID && $0.value.deviceClass == .stickS3
        })?.key,
              let characteristic = controlCharacteristics[peripheralID],
              let peripheral = peripherals[peripheralID] else {
            NSLog("BLE send \(label) skipped dev=VS-\(deviceID) (not connected)")
            return
        }
        NSLog("BLE send \(label) dev=VS-\(deviceID)")
        peripheral.writeValue(data, for: characteristic, type: .withoutResponse)
    }

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            if !isWorkspaceSleeping {
                restoreConnectedPeripherals()
            }
            scanIfReady()
        case .unknown, .resetting, .unsupported, .unauthorized, .poweredOff:
            clearConnectionState()
        @unknown default:
            clearConnectionState()
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        let localName = advertisementData[CBAdvertisementDataLocalNameKey] as? String
        guard !isWorkspaceSleeping else { return }
        // 双通道发现（对齐 Windows HandleAdvertisement）：StickS3（VS- 名称/service
        // UUID）与小米遥控器（RC- 名称/名称白名单/广告含 ATVV service UUID）。
        let serviceUUIDs = (advertisementData[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID]) ?? []
        let hasAtvvService = serviceUUIDs.contains {
            $0.uuidString.uppercased() == XiaomiAtvvProtocol.serviceUUID
        }
        guard let (deviceClass, deviceID) = classifyDiscoveredPeripheral(
            peripheral, localName: localName, hasAtvvService: hasAtvvService
        ), pairedDeviceIDs.contains(deviceID) else { return }
        if let existingPeripheral = peripherals[peripheral.identifier] {
            if existingPeripheral.state == .disconnected {
                removePeripheral(existingPeripheral)
            } else {
                return
            }
        }
        deviceClasses[peripheral.identifier] = deviceClass
        discoveredDevices[peripheral.identifier] = ConnectedVoiceStickDevice(
            name: displayName(deviceClass: deviceClass, deviceID: deviceID,
                              advertisedName: localName ?? peripheral.name ?? ""),
            deviceID: deviceID,
            deviceClass: deviceClass
        )
        peripherals[peripheral.identifier] = peripheral
        peripheral.delegate = self
        central.connect(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        connectedDevices[peripheral.identifier] = discoveredDevices[peripheral.identifier]
            ?? knownDevice(for: peripheral)
        if deviceClasses[peripheral.identifier] == nil {
            deviceClasses[peripheral.identifier] =
                connectedDevices[peripheral.identifier]?.deviceClass ?? .stickS3
        }
        let deviceClass = deviceClasses[peripheral.identifier] ?? .stickS3
        onConnectionChange?(currentConnectedDevices)
        let serviceUUID = deviceClass == .xiaomiRemote2Pro
            ? XiaomiAtvvProtocol.serviceUUID : BleProtocol.serviceUUID
        peripheral.discoverServices([CBUUID(string: serviceUUID)])
        if deviceClass == .xiaomiRemote2Pro {
            NSLog("connected RC-\(connectedDevices[peripheral.identifier]?.deviceID ?? "????")")
        }
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        removePeripheral(peripheral)
        onConnectionChange?(currentConnectedDevices)
        guard !isWorkspaceSleeping else { return }
        scanIfReady()
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        removePeripheral(peripheral)
        onConnectionChange?(currentConnectedDevices)
        guard !isWorkspaceSleeping else { return }
        scanIfReady()
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        let deviceClass = deviceClasses[peripheral.identifier] ?? .stickS3
        if deviceClass == .xiaomiRemote2Pro {
            NSLog("atvv didDiscoverServices RC-\(connectedDevices[peripheral.identifier]?.deviceID ?? discoveredDevices[peripheral.identifier]?.deviceID ?? "????") error=\(error?.localizedDescription ?? "nil") services=\((peripheral.services ?? []).map(\.uuid.uuidString))")
            let deviceID = connectedDevices[peripheral.identifier]?.deviceID ?? "????"
            // Battery Service 发现已启动后：本回调由 180F discover 触发，只推进其
            // 特征发现，不再重复 ATVV 分支（避免订阅链重跑）。服务不存在（老固件/
            // 电量特征缺失）只记日志，不阻断连接。
            if xiaomiContexts[peripheral.identifier]?.batteryDiscoveryStarted == true {
                guard error == nil,
                      let batteryService = (peripheral.services ?? []).first(where: {
                          $0.uuid == CBUUID(string: Self.batteryServiceUUID)
                      }) else {
                    NSLog("battery service unavailable RC-\(deviceID); battery level disabled")
                    return
                }
                if batteryService.characteristics == nil {
                    peripheral.discoverCharacteristics(
                        [CBUUID(string: Self.batteryLevelUUID)], for: batteryService
                    )
                }
                return
            }
            let atvvServiceUUID = CBUUID(string: XiaomiAtvvProtocol.serviceUUID)
            guard error == nil,
                  let service = (peripheral.services ?? []).first(where: { $0.uuid == atvvServiceUUID }) else {
                NSLog("atvv service discovery failed RC-\(deviceID): \(error?.localizedDescription ?? "service missing"); disconnecting")
                central?.cancelPeripheralConnection(peripheral)
                return
            }
            peripheral.discoverCharacteristics([
                CBUUID(string: XiaomiAtvvProtocol.txUUID),
                CBUUID(string: XiaomiAtvvProtocol.audioUUID),
                CBUUID(string: XiaomiAtvvProtocol.controlUUID),
            ], for: service)
            return
        }
        peripheral.services?.forEach {
            peripheral.discoverCharacteristics(nil, for: $0)
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        if (deviceClasses[peripheral.identifier] ?? .stickS3) == .xiaomiRemote2Pro {
            if service.uuid == CBUUID(string: Self.batteryServiceUUID) {
                handleXiaomiBatteryCharacteristicsDiscovery(
                    peripheral: peripheral, service: service, error: error
                )
                return
            }
            handleAtvvCharacteristicsDiscovery(peripheral: peripheral, service: service, error: error)
            return
        }
        service.characteristics?.forEach { characteristic in
            switch characteristic.uuid.uuidString.uppercased() {
            case BleProtocol.audioUUID:
                peripheral.setNotifyValue(true, for: characteristic)
            case BleProtocol.stateUUID:
                peripheral.setNotifyValue(true, for: characteristic)
            case BleProtocol.controlUUID:
                controlCharacteristics[peripheral.identifier] = characteristic
                sendUIState("ready", to: peripheral.identifier)
                sendInteractionMode(interactionMode, to: peripheral.identifier)
                sendShowIMUDebug(showIMUDebug, to: peripheral.identifier)
            case BleProtocol.otaRXUUID:
                otaCharacteristics[peripheral.identifier] = characteristic
            case BleProtocol.otaStateUUID:
                peripheral.setNotifyValue(true, for: characteristic)
            default:
                break
            }
        }
    }

    /// 小米遥控器 Battery Level 特征收集（0x180F/0x2A19，读+notify；对齐 Windows
    /// SetupXiaomiBatteryAsync）：订阅 + 初始读各拿一次电量（读值与 notify 都走
    /// didUpdateValueFor 的 battery 分支合成 battery_status）。失败只记日志不阻断连接。
    private func handleXiaomiBatteryCharacteristicsDiscovery(
        peripheral: CBPeripheral, service: CBService, error: Error?
    ) {
        let deviceID = connectedDevices[peripheral.identifier]?.deviceID ?? "????"
        guard error == nil,
              let characteristic = (service.characteristics ?? []).first(where: {
                  $0.uuid == CBUUID(string: Self.batteryLevelUUID)
              }) else {
            NSLog("battery level characteristic unavailable RC-\(deviceID): \(error?.localizedDescription ?? "missing"); battery level disabled")
            return
        }
        var context = xiaomiContexts[peripheral.identifier] ?? XiaomiPeripheralContext()
        context.batteryCharacteristic = characteristic
        xiaomiContexts[peripheral.identifier] = context
        if characteristic.properties.contains(.notify) {
            peripheral.setNotifyValue(true, for: characteristic)
        }
        // 初始读：立即拿到一次电量（对齐 Windows 的 Uncached 初始读）。
        peripheral.readValue(for: characteristic)
    }

    /// 小米遥控器（ATVV）特征收集：校验 TX writeWithoutResponse / Audio/Control
    /// notify 属性，集齐三特征后先订阅 Control（订阅链在
    /// didUpdateNotificationStateFor 推进：Control → Audio → 创建会话）。
    /// 特征缺失/属性不符：NSLog + 断开（对齐 Windows fail 语义）。
    private func handleAtvvCharacteristicsDiscovery(peripheral: CBPeripheral, service: CBService, error: Error?) {
        let deviceID = connectedDevices[peripheral.identifier]?.deviceID ?? "????"
        guard service.uuid == CBUUID(string: XiaomiAtvvProtocol.serviceUUID) else { return }
        func fail(_ message: String) {
            NSLog("atvv characteristic setup failed RC-\(deviceID): \(message); disconnecting")
            central?.cancelPeripheralConnection(peripheral)
        }
        if let error {
            fail(error.localizedDescription)
            return
        }
        var context = xiaomiContexts[peripheral.identifier] ?? XiaomiPeripheralContext()
        for characteristic in service.characteristics ?? [] {
            switch characteristic.uuid.uuidString.uppercased() {
            case XiaomiAtvvProtocol.txUUID:
                guard characteristic.properties.contains(.writeWithoutResponse) else {
                    fail("tx missing writeWithoutResponse")
                    return
                }
                context.txCharacteristic = characteristic
            case XiaomiAtvvProtocol.audioUUID:
                guard characteristic.properties.contains(.notify) else {
                    fail("audio missing notify")
                    return
                }
                context.audioCharacteristic = characteristic
            case XiaomiAtvvProtocol.controlUUID:
                guard characteristic.properties.contains(.notify) else {
                    fail("control missing notify")
                    return
                }
                context.controlCharacteristic = characteristic
            default:
                break
            }
        }
        guard context.txCharacteristic != nil, context.audioCharacteristic != nil,
              let control = context.controlCharacteristic else {
            fail("characteristics incomplete")
            return
        }
        xiaomiContexts[peripheral.identifier] = context
        NSLog("subscribing atvv control notifications RC-\(deviceID)")
        armXiaomiSubscribeTimeout(for: peripheral)
        peripheral.setNotifyValue(true, for: control)
    }

    /// ATVV 订阅链：Control 订阅成功后订阅 Audio，Audio 就绪后创建并启动会话。
    /// StickS3 特征不处理（沿用现状不检查订阅结果）。
    func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?) {
        guard let context = xiaomiContexts[peripheral.identifier] else { return }
        let deviceID = connectedDevices[peripheral.identifier]?.deviceID ?? "????"
        if characteristic == context.controlCharacteristic {
            guard error == nil, characteristic.isNotifying else {
                NSLog("atvv control subscribe failed RC-\(deviceID): \(error?.localizedDescription ?? "not notifying"); disconnecting")
                cancelXiaomiSubscribeTimeout(for: peripheral.identifier)
                central?.cancelPeripheralConnection(peripheral)
                return
            }
            if let audio = context.audioCharacteristic {
                NSLog("subscribing atvv audio notifications RC-\(deviceID)")
                peripheral.setNotifyValue(true, for: audio)
            }
        } else if characteristic == context.audioCharacteristic {
            guard error == nil, characteristic.isNotifying else {
                NSLog("atvv audio subscribe failed RC-\(deviceID): \(error?.localizedDescription ?? "not notifying"); disconnecting")
                cancelXiaomiSubscribeTimeout(for: peripheral.identifier)
                central?.cancelPeripheralConnection(peripheral)
                return
            }
            // 订阅链完成：撤兜底定时器。
            cancelXiaomiSubscribeTimeout(for: peripheral.identifier)
            startXiaomiSessionIfNeeded(peripheral)
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard let data = characteristic.value else { return }
        // ATVV 两路分发（按 per-peripheral 存的特征句柄匹配）：Control/Audio 字节
        // 喂会话状态机并分发动作；F5 锚点在驱动会话前刷新（对齐 Windows）。
        if let context = xiaomiContexts[peripheral.identifier] {
            let now = Self.nowMs()
            if characteristic == context.controlCharacteristic {
                if let opcode = data.first,
                   opcode == XiaomiAtvvProtocol.controlMicOpen ||
                    opcode == XiaomiAtvvProtocol.controlStreamStart {
                    micOpenAnchor.note(now)
                }
                driveXiaomiSession(peripheral) { $0.handleControlCommand(data, nowMs: now) }
            } else if characteristic == context.audioCharacteristic {
                micOpenAnchor.note(now)
                driveXiaomiSession(peripheral) { $0.handleAudioData(data, nowMs: now) }
            } else if characteristic == context.batteryCharacteristic {
                // 标准 Battery Level 0x2A19：首字节即百分比，合成 battery_status 事件
                // 走 onStateEvent（对齐 Windows SetupXiaomiBatteryAsync 的读/notify 处理）。
                if let level = data.first {
                    onStateEvent?(peripheral.identifier, StateEvent(
                        event: "battery_status", button: nil, sessionID: nil, durationMs: nil,
                        hardware: nil, firmwareVersion: nil, buttons: nil, uiStates: nil,
                        batteryLevel: Int(level)
                    ))
                }
            }
            return
        }
        switch characteristic.uuid.uuidString.uppercased() {
        case BleProtocol.audioUUID:
            if let frame = BleProtocol.parseAudioFrame(data) {
                onAudioFrame?(peripheral.identifier, frame)
            }
        case BleProtocol.stateUUID:
            // 分发顺序对齐 Windows：StateEvent（power_mgmt 返回 nil）→ power_log
            // 分片（无 "event" 键）→ power_mgmt 事件。
            if let event = BleProtocol.parseStateEvent(data) {
                onStateEvent?(peripheral.identifier, event)
            } else if let fragment = BleProtocol.parsePowerLogFragment(data) {
                onPowerLogFragment?(peripheral.identifier, fragment)
            } else if let powerMgmt = BleProtocol.parsePowerMgmtEvent(data) {
                onPowerMgmtEvent?(peripheral.identifier, powerMgmt)
            }
        case BleProtocol.otaStateUUID:
            if let event = BleProtocol.parseFirmwareOTAStateEvent(data) {
                handleFirmwareUpdateStateEvent(event)
            }
        default:
            break
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        guard characteristic.uuid.uuidString.uppercased() == BleProtocol.otaRXUUID,
              firmwareUpdateSession?.peripheralID == peripheral.identifier else {
            return
        }
        if let error {
            failFirmwareUpdate(FirmwareUpdateError.peripheralWriteFailed(error.localizedDescription))
            return
        }
        sendNextFirmwareUpdateFrame()
    }

    func peripheralIsReady(toSendWriteWithoutResponse peripheral: CBPeripheral) {
        guard firmwareUpdateSession?.peripheralID == peripheral.identifier else {
            return
        }
        sendNextFirmwareUpdateFrame()
    }

    /// 广告判定（对齐 Windows HandleAdvertisement）：返回（设备类别，归一化 4 位
    /// 大写 hex ID）。VS-/RC- 前缀名从名称取 ID；小米白名单名或仅含 ATVV service
    /// UUID 的广告（名称无 ID）须由配对条目按 peripheral UUID 反查。
    /// pairedDeviceIDs 门控由调用方做。
    private func classifyDiscoveredPeripheral(_ peripheral: CBPeripheral, localName: String?,
                                              hasAtvvService: Bool) -> (DeviceClass, String)? {
        let advertisedName = localName ?? peripheral.name ?? ""
        guard let deviceClass = BleProtocol.deviceClass(forName: advertisedName)
                ?? (hasAtvvService ? .xiaomiRemote2Pro : nil) else {
            return nil
        }
        let upper = advertisedName.trimmingCharacters(in: .whitespacesAndNewlines).uppercased()
        if upper.hasPrefix("VS-") || upper.hasPrefix("RC-") {
            let suffix = upper.dropFirst(3).prefix(4)
            guard suffix.count == 4, suffix.allSatisfy(\.isHexDigit) else { return nil }
            return (deviceClass, String(suffix))
        }
        guard deviceClass == .xiaomiRemote2Pro,
              let entry = pairedDeviceEntry(forPeripheralUUID: peripheral.identifier.uuidString) else {
            return nil
        }
        return (.xiaomiRemote2Pro, entry.deviceID)
    }

    /// 菜单展示名：RC 设备恒为 RC-<id>（原名是白名单名，不带 ID）；StickS3 沿用
    /// 广告名（空则 VS-<id>）。
    private func displayName(deviceClass: DeviceClass, deviceID: String, advertisedName: String) -> String {
        switch deviceClass {
        case .stickS3:
            return advertisedName.isEmpty ? "VS-\(deviceID)" : advertisedName
        case .xiaomiRemote2Pro:
            return "RC-\(deviceID)"
        }
    }

    /// 按 CoreBluetooth 外设 UUID 反查配对条目（addr 段存大写 uuidString）。
    private func pairedDeviceEntry(forPeripheralUUID uuidString: String) -> PairedDeviceEntry? {
        let target = uuidString.uppercased()
        return pairedDevicesProvider?().first { $0.address.uppercased() == target }
    }

    private func pairedDeviceEntry(forID deviceID: String) -> PairedDeviceEntry? {
        pairedDevicesProvider?().first { $0.deviceID == deviceID }
    }

    private var currentConnectedDevices: [ConnectedVoiceStickDevice] {
        connectedDevices.values.sorted { $0.name < $1.name }
    }

    private func sendNextFirmwareUpdateFrame() {
        guard var session = firmwareUpdateSession,
              let peripheral = peripherals[session.peripheralID],
              let characteristic = otaCharacteristics[session.peripheralID] else {
            failFirmwareUpdate(FirmwareUpdateError.otaCharacteristicUnavailable)
            return
        }

        if !session.began {
            let payload = BleProtocol.otaBeginPayload(
                imageSize: UInt32(session.image.count),
                transferID: session.transferID
            )
            session.began = true
            firmwareUpdateSession = session
            peripheral.writeValue(payload, for: characteristic, type: .withResponse)
        } else if session.offset < session.image.count {
            while session.offset < session.image.count && peripheral.canSendWriteWithoutResponse {
                let end = min(session.offset + session.chunkSize, session.image.count)
                let chunk = session.image.subdata(in: session.offset..<end)
                let payload = BleProtocol.otaDataPayload(
                    transferID: session.transferID,
                    offset: UInt32(session.offset),
                    chunk: chunk
                )
                peripheral.writeValue(payload, for: characteristic, type: .withoutResponse)
                session.offset = end
                if session.offset - session.lastQueuedProgressOffset >= 64 * 1024 ||
                    session.offset == session.image.count {
                    session.lastQueuedProgressOffset = session.offset
                    session.progress(FirmwareUpdateProgress(
                        writtenBytes: session.offset,
                        totalBytes: session.image.count,
                        isDeviceConfirmed: false
                    ))
                }
            }
            firmwareUpdateSession = session
            if session.offset == session.image.count {
                sendNextFirmwareUpdateFrame()
            }
        } else if !session.ended {
            let payload = BleProtocol.otaEndPayload(
                transferID: session.transferID,
                imageSize: UInt32(session.image.count)
            )
            session.ended = true
            firmwareUpdateSession = session
            peripheral.writeValue(payload, for: characteristic, type: .withResponse)
        } else {
            return
        }
    }

    private func handleFirmwareUpdateStateEvent(_ event: FirmwareOTAStateEvent) {
        guard let session = firmwareUpdateSession else { return }
        if let transferID = event.transferID, transferID != session.transferID {
            return
        }

        switch event.event {
        case "progress":
            if let written = event.written, let size = event.size, size > 0 {
                session.progress(FirmwareUpdateProgress(
                    writtenBytes: Int(written),
                    totalBytes: Int(size),
                    isDeviceConfirmed: true
                ))
            }
        case "done":
            firmwareUpdateSession = nil
            session.progress(FirmwareUpdateProgress(
                writtenBytes: session.image.count,
                totalBytes: session.image.count,
                isDeviceConfirmed: true
            ))
            session.completion(.success(()))
        case "error":
            failFirmwareUpdate(FirmwareUpdateError.deviceError(event.code ?? "unknown"))
        default:
            break
        }
    }

    private func failFirmwareUpdate(_ error: Error) {
        guard let session = firmwareUpdateSession else { return }
        if let peripheral = peripherals[session.peripheralID],
           let characteristic = otaCharacteristics[session.peripheralID] {
            let payload = BleProtocol.otaAbortPayload(transferID: session.transferID)
            peripheral.writeValue(payload, for: characteristic, type: .withoutResponse)
        }
        firmwareUpdateSession = nil
        session.completion(.failure(error))
    }

    private func scanIfReady() {
        guard let central, central.state == .poweredOn else { return }
        guard !isWorkspaceSleeping else {
            central.stopScan()
            return
        }
        if pairedDeviceIDs.isEmpty {
            central.stopScan()
        } else {
            central.scanForPeripherals(withServices: [
                CBUUID(string: BleProtocol.serviceUUID),
                CBUUID(string: XiaomiAtvvProtocol.serviceUUID),
            ])
        }
    }

    private func restoreConnectedPeripherals() {
        guard let central, central.state == .poweredOn, !isWorkspaceSleeping, !pairedDeviceIDs.isEmpty else { return }
        let stickServiceUUID = CBUUID(string: BleProtocol.serviceUUID)
        let atvvServiceUUID = CBUUID(string: XiaomiAtvvProtocol.serviceUUID)
        let restoredPeripherals = central.retrieveConnectedPeripherals(withServices: [stickServiceUUID, atvvServiceUUID])
        NSLog("restoreConnectedPeripherals: system-connected=\(restoredPeripherals.count) pairedIDs=\(pairedDeviceIDs.count)")
        var didRestore = false
        for peripheral in restoredPeripherals {
            let device = knownDevice(for: peripheral)
            NSLog("restored peripheral name=\(peripheral.name ?? "nil") id=\(peripheral.identifier) state=\(peripheral.state.rawValue) known=\(device.map { "\($0.name)/\($0.deviceClass)" } ?? "nil")")
            guard let device else {
                continue
            }
            discoveredDevices[peripheral.identifier] = device
            deviceClasses[peripheral.identifier] = device.deviceClass
            peripherals[peripheral.identifier] = peripheral
            peripheral.delegate = self
            if peripheral.state == .connected {
                connectedDevices[peripheral.identifier] = device
                peripheral.discoverServices([
                    device.deviceClass == .xiaomiRemote2Pro ? atvvServiceUUID : stickServiceUUID,
                ])
            } else {
                // 取回时可能已断开（遥控器休眠）：走正常连接，didConnect 再按类发现服务
                central.connect(peripheral)
            }
            didRestore = true
        }
        // 盲区补偿：遥控器可能不在广播里带 ATVV UUID，按配对条目存的
        // CoreBluetooth 外设 UUID 直接取回并重连（对齐 Windows ConnectPairedDevice）。
        if reconnectPairedXiaomiPeripherals(central) {
            didRestore = true
        }
        if didRestore {
            onConnectionChange?(currentConnectedDevices)
        }
    }

    /// 对配对条目中的 RC 设备按存的 peripheral UUID 直接尝试重连。
    /// 返回是否有设备进入连接/恢复流程。
    @discardableResult
    private func reconnectPairedXiaomiPeripherals(_ central: CBCentralManager) -> Bool {
        guard let entries = pairedDevicesProvider?() else { return false }
        var didInitiate = false
        for entry in entries where entry.hardware == Self.hardwareXiaomiRemote2Pro {
            guard let uuid = UUID(uuidString: entry.address) else {
                NSLog("reconnect RC-\(entry.deviceID) skipped: addr not a UUID: '\(entry.address)'")
                continue
            }
            guard peripherals[uuid] == nil else {
                NSLog("reconnect RC-\(entry.deviceID) skipped: already tracked")
                continue
            }
            guard let peripheral = central.retrievePeripherals(withIdentifiers: [uuid]).first else {
                NSLog("reconnect RC-\(entry.deviceID) skipped: retrievePeripherals empty for \(entry.address)")
                continue
            }
            let device = ConnectedVoiceStickDevice(
                name: "RC-\(entry.deviceID)", deviceID: entry.deviceID, deviceClass: .xiaomiRemote2Pro
            )
            discoveredDevices[uuid] = device
            deviceClasses[uuid] = .xiaomiRemote2Pro
            peripherals[uuid] = peripheral
            peripheral.delegate = self
            if peripheral.state == .connected {
                NSLog("reconnect RC-\(entry.deviceID): already system-connected, discovering ATVV")
                connectedDevices[uuid] = device
                peripheral.discoverServices([CBUUID(string: XiaomiAtvvProtocol.serviceUUID)])
            } else {
                NSLog("reconnecting paired RC-\(entry.deviceID) by peripheral uuid")
                central.connect(peripheral)
            }
            didInitiate = true
        }
        return didInitiate
    }

    private func knownDevice(for peripheral: CBPeripheral) -> ConnectedVoiceStickDevice? {
        if let device = discoveredDevices[peripheral.identifier] {
            return device
        }
        // RC：按 CoreBluetooth 外设 UUID 反查配对条目（macOS 配对时 addr 存 uuidString）。
        if let entry = pairedDeviceEntry(forPeripheralUUID: peripheral.identifier.uuidString),
           entry.hardware == Self.hardwareXiaomiRemote2Pro {
            return ConnectedVoiceStickDevice(
                name: "RC-\(entry.deviceID)", deviceID: entry.deviceID, deviceClass: .xiaomiRemote2Pro
            )
        }
        if let name = peripheral.name, !name.isEmpty {
            if let deviceID = Self.deviceID(from: name) {
                return ConnectedVoiceStickDevice(name: name, deviceID: deviceID, deviceClass: .stickS3)
            }
            if BleProtocol.deviceClass(forName: name) == .xiaomiRemote2Pro {
                // RC-XXXX 名可直接取 id；白名单名（MI RC 等）无 id，只能等配对条目
                //（上方已查，未配对则无 id）。
                let upper = name.trimmingCharacters(in: .whitespacesAndNewlines).uppercased()
                guard upper.hasPrefix("RC-") else { return nil }
                let suffix = upper.dropFirst(3).prefix(4)
                guard suffix.count == 4, suffix.allSatisfy(\.isHexDigit) else { return nil }
                return ConnectedVoiceStickDevice(
                    name: "RC-\(suffix)", deviceID: String(suffix), deviceClass: .xiaomiRemote2Pro
                )
            }
        }
        guard pairedDeviceIDs.count == 1, let deviceID = pairedDeviceIDs.first else {
            return nil
        }
        let deviceClass: DeviceClass =
            pairedDeviceEntry(forID: deviceID)?.hardware == Self.hardwareXiaomiRemote2Pro
                ? .xiaomiRemote2Pro : .stickS3
        return ConnectedVoiceStickDevice(
            name: deviceClass == .xiaomiRemote2Pro ? "RC-\(deviceID)" : (peripheral.name ?? "VS-\(deviceID)"),
            deviceID: deviceID,
            deviceClass: deviceClass
        )
    }

    private func clearConnectionState() {
        central?.stopScan()
        if firmwareUpdateSession != nil {
            failFirmwareUpdate(FirmwareUpdateError.noConnectedDevice)
        }
        // RC 会话尽力 MIC_CLOSE（休眠主动断开场景此刻链路尚在，写完即清）。
        for peripheral in peripherals.values {
            stopXiaomiSessionBestEffort(for: peripheral)
        }
        peripherals.removeAll()
        discoveredDevices.removeAll()
        connectedDevices.removeAll()
        controlCharacteristics.removeAll()
        otaCharacteristics.removeAll()
        deviceClasses.removeAll()
        xiaomiContexts.keys.forEach { cancelXiaomiSubscribeTimeout(for: $0) }
        xiaomiContexts.removeAll()
        syncXiaomiTickTimer()
        onConnectionChange?([])
    }

    private func removePeripheral(_ peripheral: CBPeripheral) {
        stopXiaomiSessionBestEffort(for: peripheral)
        peripherals.removeValue(forKey: peripheral.identifier)
        discoveredDevices.removeValue(forKey: peripheral.identifier)
        connectedDevices.removeValue(forKey: peripheral.identifier)
        controlCharacteristics.removeValue(forKey: peripheral.identifier)
        otaCharacteristics.removeValue(forKey: peripheral.identifier)
        deviceClasses.removeValue(forKey: peripheral.identifier)
        cancelXiaomiSubscribeTimeout(for: peripheral.identifier)
        xiaomiContexts.removeValue(forKey: peripheral.identifier)
        syncXiaomiTickTimer()
        if firmwareUpdateSession?.peripheralID == peripheral.identifier {
            failFirmwareUpdate(FirmwareUpdateError.noConnectedDevice)
        }
    }

    // ---- 小米 ATVV 会话辅助（对齐 Windows DriveXiaomiSession/DispatchXiaomiActions）----

    /// 会话线程契约：全部入口在 CoreBluetooth delegate queue（.main）串行调用。
    private func driveXiaomiSession(_ peripheral: CBPeripheral,
                                    _ entry: (XiaomiAtvvSession) -> [XiaomiAtvvAction]) {
        guard let session = xiaomiContexts[peripheral.identifier]?.session else { return }
        let actions = entry(session)
        if !actions.isEmpty {
            dispatchXiaomiActions(actions, peripheral: peripheral)
        }
    }

    private func dispatchXiaomiActions(_ actions: [XiaomiAtvvAction], peripheral: CBPeripheral) {
        let peripheralID = peripheral.identifier
        let deviceID = connectedDevices[peripheralID]?.deviceID ?? "????"
        for action in actions {
            switch action {
            case .writeTx(let data):
                guard let tx = xiaomiContexts[peripheralID]?.txCharacteristic else { break }
                NSLog("atvv tx write RC-\(deviceID) len=\(data.count)")
                peripheral.writeValue(data, for: tx, type: .withoutResponse)
            case .stateEvent(let event):
                NSLog("atvv event RC-\(deviceID) type=\(event.event)")
                onStateEvent?(peripheralID, event)
            case .audioFrame(let frame):
                onAudioFrame?(peripheralID, frame)
            case .error(let code):
                // 对齐 Windows DispatchXiaomiActions：仅 caps_timeout（多为半开链路）
                // 拆链走扫描快速重连；其他错误（如 unsupported_codec）会话已入 error
                // 终态不会再有动作，保持连接静止只上报——拆链会被 scanIfReady 立即
                // 重扫重连，陷入无限循环（macOS 无 on_connection_error 通道）。
                if code == "caps_timeout" {
                    NSLog("xiaomi session error RC-\(deviceID) code=caps_timeout; disconnecting")
                    central?.cancelPeripheralConnection(peripheral)
                } else {
                    NSLog("xiaomi session error RC-\(deviceID) code=\(code); keeping connection")
                }
            }
        }
    }

    /// Audio/Control notify 订阅完成后创建并启动会话（主线程，构造可抛：opus
    /// encoder 失败则断开）。合成 device_info 对齐 Windows：协调器据此登记
    /// hardware 能力标签，固件版本留空（小米遥控器没有 VoiceStick 固件概念）。
    private func startXiaomiSessionIfNeeded(_ peripheral: CBPeripheral) {
        let peripheralID = peripheral.identifier
        guard var context = xiaomiContexts[peripheralID], context.session == nil else { return }
        let deviceID = connectedDevices[peripheralID]?.deviceID ?? "????"
        let options = xiaomiOptionsResolver?(deviceID) ?? XiaomiAtvvSession.Options()
        let session: XiaomiAtvvSession
        do {
            session = try XiaomiAtvvSession(options: options)
        } catch {
            NSLog("xiaomi session create failed RC-\(deviceID): \(error.localizedDescription); disconnecting")
            central?.cancelPeripheralConnection(peripheral)
            return
        }
        context.session = session
        xiaomiContexts[peripheralID] = context
        NSLog("xiaomi session created RC-\(deviceID)")
        onStateEvent?(peripheralID, StateEvent(
            event: "device_info", button: nil, sessionID: nil, durationMs: nil,
            hardware: Self.hardwareXiaomiRemote2Pro, firmwareVersion: nil,
            buttons: nil, uiStates: nil
        ))
        driveXiaomiSession(peripheral) { $0.start(nowMs: Self.nowMs()) }
        syncXiaomiTickTimer()
        // 会话建立后发现可选 Battery Service（0x180F/0x2A19，订阅+初始读）；
        // 失败只记日志不阻断连接（对齐 Windows SetupXiaomiBatteryAsync）。
        context = xiaomiContexts[peripheralID] ?? context
        if !context.batteryDiscoveryStarted {
            context.batteryDiscoveryStarted = true
            xiaomiContexts[peripheralID] = context
            peripheral.discoverServices([CBUUID(string: Self.batteryServiceUUID)])
        }
    }

    /// ATVV 订阅链超时兜底（对齐 Windows kSubscribeTimeout 防御）：订阅推进完全
    /// 依赖 didUpdateNotificationStateFor 回调必达，回调丢失会永久挂起。发起
    /// Control 订阅时武装一次性定时器；链完成/断开/清理时取消。超时未到则拆链，
    /// 由扫描重连兜底。
    private func armXiaomiSubscribeTimeout(for peripheral: CBPeripheral) {
        let peripheralID = peripheral.identifier
        cancelXiaomiSubscribeTimeout(for: peripheralID)
        let deviceID = connectedDevices[peripheralID]?.deviceID ?? "????"
        let timer = Timer(timeInterval: Self.atvvSubscribeTimeout, repeats: false) { [weak self] _ in
            // 上下文已清（断开/清理）或会话已建（链已完成但漏取消）→ 不动作。
            guard let self, let peripheral = self.peripherals[peripheralID],
                  let context = self.xiaomiContexts[peripheralID], context.session == nil else { return }
            NSLog("atvv subscribe timeout RC-\(deviceID) after \(Int(Self.atvvSubscribeTimeout * 1000))ms; disconnecting")
            self.central?.cancelPeripheralConnection(peripheral)
        }
        RunLoop.main.add(timer, forMode: .common)
        xiaomiContexts[peripheralID]?.subscribeTimeoutTimer = timer
    }

    private func cancelXiaomiSubscribeTimeout(for peripheralID: UUID) {
        xiaomiContexts[peripheralID]?.subscribeTimeoutTimer?.invalidate()
        xiaomiContexts[peripheralID]?.subscribeTimeoutTimer = nil
    }

    /// 断开/清理前尽力停会话：mic 开着会发 MIC_CLOSE writeTx（尽力写，写完即清）。
    private func stopXiaomiSessionBestEffort(for peripheral: CBPeripheral) {
        guard let session = xiaomiContexts[peripheral.identifier]?.session else { return }
        let actions = session.stop(nowMs: Self.nowMs())
        if !actions.isEmpty {
            dispatchXiaomiActions(actions, peripheral: peripheral)
        }
    }

    /// 50ms tick 泵（对齐 Windows kXiaomiSessionTickMs）：有存活 RC 会话才启动，
    /// 全断即停。主 RunLoop common mode（会话线程契约 = 主线程，与 delegate
    /// queue 一致；菜单打开时仍需驱动长按阈值/尾包宽限）。
    private func syncXiaomiTickTimer() {
        let hasSession = xiaomiContexts.values.contains { $0.session != nil }
        if hasSession, xiaomiTickTimer == nil {
            let timer = Timer(timeInterval: 0.05, repeats: true) { [weak self] _ in
                self?.tickXiaomiSessions()
            }
            RunLoop.main.add(timer, forMode: .common)
            xiaomiTickTimer = timer
        } else if !hasSession {
            xiaomiTickTimer?.invalidate()
            xiaomiTickTimer = nil
        }
    }

    private func tickXiaomiSessions() {
        let now = Self.nowMs()
        for (peripheralID, context) in xiaomiContexts where context.session != nil {
            guard let peripheral = peripherals[peripheralID] else { continue }
            driveXiaomiSession(peripheral) { $0.tick(nowMs: now) }
        }
    }

    @objc private func workspaceWillSleep() {
        isWorkspaceSleeping = true
        central?.stopScan()
        sendUIState("ready")
        for peripheral in peripherals.values where peripheral.state != .disconnected {
            central?.cancelPeripheralConnection(peripheral)
        }
        clearConnectionState()
    }

    @objc private func workspaceDidWake() {
        isWorkspaceSleeping = false
        restoreConnectedPeripherals()
        scanIfReady()
    }

    static func deviceID(from name: String) -> String? {
        let upper = name.uppercased()
        guard upper.hasPrefix("VS-") else { return nil }
        let id = String(upper.dropFirst(3).prefix(4))
        guard id.count == 4, id.allSatisfy(\.isHexDigit) else { return nil }
        return id
    }
}
