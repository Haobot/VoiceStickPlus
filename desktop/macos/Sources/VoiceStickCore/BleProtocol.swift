import CryptoKit
import Foundation

public struct AudioFrame {
    public let sessionID: UInt32
    public let seq: UInt32
    public let flags: UInt8
    public let payload: Data

    public init(sessionID: UInt32, seq: UInt32, flags: UInt8, payload: Data) {
        self.sessionID = sessionID
        self.seq = seq
        self.flags = flags
        self.payload = payload
    }

    public var isStart: Bool { flags & 0x01 != 0 }
    public var isEnd: Bool { flags & 0x02 != 0 }
}

public struct StateEvent: Decodable {
    public let event: String
    public let button: String?
    public let sessionID: UInt32?
    public let durationMs: UInt32?
    public let hardware: String?
    public let firmwareVersion: String?
    public let buttons: [String]?
    public let uiStates: [String]?
    /// 按键来源（对齐 Windows StateEvent.source）：主键/侧键无此字段，
    /// 编码器按键事件带 "encoder"，用于路由到编码器按键处理器。
    public let source: String?
    /// encoder_rotate 事件字段（10ms 窗口聚合的格数与方向 "cw"/"ccw"）。
    public let steps: UInt32?
    public let direction: String?
    /// encoder_status 事件字段：编码器是否存在（老固件无此事件，消费侧默认 true）。
    public let encoderPresent: Bool?
    /// battery_status 事件字段（百分比由固件计算；小米遥控器由桌面端读 0x2A19 合成）。
    public let batteryLevel: Int?
    public let batteryCharging: Bool?
    public let batteryUsbPowered: Bool?
    /// gateway_key 事件字段（P1 隧道融合）：被路由为 software 的小米遥控器键名与
    /// 按下/抬起沿（pressed/released 成对到达，链路中断由固件合成抬起沿）。
    public let gatewayKey: String?
    public let gatewayPressed: Bool?
    /// gateway_status 事件字段：固件当前 "gateway"/"normal"（独立小帧，老固件不发）。
    public let gatewayMode: String?
    /// gateway_keymap 报告帧字段：路由表回报（gateway_keymap_set/get 的应答）。
    public let keymapRoutes: [GatewayKeymapRoute]?

    public init(event: String, button: String?, sessionID: UInt32?, durationMs: UInt32?,
                hardware: String?, firmwareVersion: String?, buttons: [String]?, uiStates: [String]?,
                source: String? = nil, steps: UInt32? = nil, direction: String? = nil,
                encoderPresent: Bool? = nil, batteryLevel: Int? = nil,
                batteryCharging: Bool? = nil, batteryUsbPowered: Bool? = nil,
                gatewayKey: String? = nil, gatewayPressed: Bool? = nil,
                gatewayMode: String? = nil, keymapRoutes: [GatewayKeymapRoute]? = nil) {
        self.event = event
        self.button = button
        self.sessionID = sessionID
        self.durationMs = durationMs
        self.hardware = hardware
        self.firmwareVersion = firmwareVersion
        self.buttons = buttons
        self.uiStates = uiStates
        self.source = source
        self.steps = steps
        self.direction = direction
        self.encoderPresent = encoderPresent
        self.batteryLevel = batteryLevel
        self.batteryCharging = batteryCharging
        self.batteryUsbPowered = batteryUsbPowered
        self.gatewayKey = gatewayKey
        self.gatewayPressed = gatewayPressed
        self.gatewayMode = gatewayMode
        self.keymapRoutes = keymapRoutes
    }

    enum CodingKeys: String, CodingKey {
        case event
        case button
        case sessionID = "session_id"
        case durationMs = "duration_ms"
        case hardware
        case firmwareVersion = "firmware_version"
        case buttons
        case uiStates = "ui_states"
        case source
        case steps
        case direction
        case encoderPresent = "present"
        case batteryLevel = "level"
        case batteryCharging = "charging"
        case batteryUsbPowered = "usb_powered"
        case gatewayKey = "key"
        case gatewayPressed = "pressed"
        case gatewayMode = "mode"
        case keymapRoutes = "routes"
    }
}

/// power_log 分片帧（state_tx 上行，无 "event" 键）：
/// `{"power_log":{"seq":N,"offset":N,"total":N,"eof":0|1,"data":"<base64>"}}`。
public struct PowerLogFragment: Decodable {
    public let seq: UInt32
    public let offset: UInt32
    public let total: UInt32
    public let eof: Bool
    public let data: Data

    private struct Container: Decodable {
        struct Body: Decodable {
            let seq: UInt32?
            let offset: UInt32?
            let total: UInt32?
            let eof: Int?
            let data: String?
        }
        let power_log: Body?
    }

    static func decode(jsonPayload: Data) -> PowerLogFragment? {
        guard let container = try? JSONDecoder().decode(Container.self, from: jsonPayload),
              let body = container.power_log,
              let offset = body.offset, let total = body.total,
              let dataString = body.data,
              let decoded = Data(base64Encoded: dataString) else {
            return nil
        }
        return PowerLogFragment(
            seq: body.seq ?? 0,
            offset: offset,
            total: total,
            eof: (body.eof ?? 0) != 0,
            data: decoded
        )
    }
}

/// 供电管理事件（state_tx 上行）：`{"event":"power_mgmt","usb_auto_off":bool}`。
/// 固件连接时主动推送，usb_auto_off set 命令后回推确认。
public struct PowerMgmtEvent {
    public let usbAutoOff: Bool

    static func decode(jsonPayload: Data) -> PowerMgmtEvent? {
        guard let object = try? JSONSerialization.jsonObject(with: jsonPayload) as? [String: Any],
              (object["event"] as? String) == "power_mgmt",
              let usbAutoOff = object["usb_auto_off"] as? Bool else {
            return nil
        }
        return PowerMgmtEvent(usbAutoOff: usbAutoOff)
    }
}

public struct FirmwareOTAStateEvent: Decodable {
    public let event: String
    public let transferID: UInt32?
    public let written: UInt32?
    public let size: UInt32?
    public let code: String?
    public let espErr: Int?
    public let rebootMs: Int?

    public init(event: String, transferID: UInt32?, written: UInt32?, size: UInt32?,
                code: String?, espErr: Int?, rebootMs: Int?) {
        self.event = event
        self.transferID = transferID
        self.written = written
        self.size = size
        self.code = code
        self.espErr = espErr
        self.rebootMs = rebootMs
    }

    enum CodingKeys: String, CodingKey {
        case event
        case transferID = "transfer_id"
        case written
        case size
        case code
        case espErr = "esp_err"
        case rebootMs = "reboot_ms"
    }
}

/// 输入设备类别：自研 StickS3（VS-XXXX）或小米遥控器（RC-XXXX，ATVV 协议）。
public enum DeviceClass {
    case stickS3
    case xiaomiRemote2Pro
}

public enum BleProtocol {
    public static let serviceUUID = "8F2F0B84-6E6F-4B23-88F7-3A3CEAFC5100"
    public static let audioUUID = "8F2F0B84-6E6F-4B23-88F7-3A3CEAFC5101"
    public static let stateUUID = "8F2F0B84-6E6F-4B23-88F7-3A3CEAFC5102"
    public static let controlUUID = "8F2F0B84-6E6F-4B23-88F7-3A3CEAFC5103"
    public static let otaRXUUID = "8F2F0B84-6E6F-4B23-88F7-3A3CEAFC5104"
    public static let otaStateUUID = "8F2F0B84-6E6F-4B23-88F7-3A3CEAFC5105"

    public static let otaTypeBegin: UInt8 = 0x20
    public static let otaTypeData: UInt8 = 0x21
    public static let otaTypeEnd: UInt8 = 0x22
    public static let otaTypeAbort: UInt8 = 0x23
    public static let otaTypeState: UInt8 = 0x30

    public static func parseAudioFrame(_ data: Data) -> AudioFrame? {
        guard data.count >= 16 else { return nil }
        guard data[0] == 1, data[1] == 0x01 else { return nil }

        let headerLength = UInt16(littleEndianBytes: data[2..<4])
        guard headerLength == 16, data.count >= Int(headerLength) else { return nil }

        let sessionID = UInt32(littleEndianBytes: data[4..<8])
        let seq = UInt32(littleEndianBytes: data[8..<12])
        let flags = data[12]
        let payloadLength = Int(UInt16(littleEndianBytes: data[14..<16]))
        guard data.count >= 16 + payloadLength else { return nil }

        return AudioFrame(
            sessionID: sessionID,
            seq: seq,
            flags: flags,
            payload: data.subdata(in: 16..<(16 + payloadLength))
        )
    }

    public static func parseStateEvent(_ data: Data) -> StateEvent? {
        guard data.count >= 4, data[0] == 1, data[1] == 0x10 else { return nil }
        let payloadLength = Int(UInt16(littleEndianBytes: data[2..<4]))
        guard data.count >= 4 + payloadLength else { return nil }
        let payload = data.subdata(in: 4..<(4 + payloadLength))
        guard let event = try? JSONDecoder().decode(StateEvent.self, from: payload) else { return nil }
        // 对齐 Windows ParseStateEvent：power_mgmt 事件不属于 StateEvent 通道，
        // 返回 nil 让分发链继续走 ParsePowerMgmtEvent（power_log 分片无 "event" 键，
        // 天然解码失败落到 ParsePowerLogFragment）。
        guard event.event != "power_mgmt" else { return nil }
        return event
    }

    /// state_tx 负载（4 字节帧头后的 JSON）辅助：剥帧头。
    private static func statePayload(_ data: Data) -> Data? {
        guard data.count >= 4, data[0] == 1, data[1] == 0x10 else { return nil }
        let payloadLength = Int(UInt16(littleEndianBytes: data[2..<4]))
        guard data.count >= 4 + payloadLength else { return nil }
        return data.subdata(in: 4..<(4 + payloadLength))
    }

    /// 解析 power_log 分片帧（对齐 Windows ParsePowerLogFragment）。
    public static func parsePowerLogFragment(_ data: Data) -> PowerLogFragment? {
        guard let payload = statePayload(data) else { return nil }
        return PowerLogFragment.decode(jsonPayload: payload)
    }

    /// 解析 power_mgmt 事件（对齐 Windows ParsePowerMgmtEvent）。
    public static func parsePowerMgmtEvent(_ data: Data) -> PowerMgmtEvent? {
        guard let payload = statePayload(data) else { return nil }
        return PowerMgmtEvent.decode(jsonPayload: payload)
    }

    public static func parseFirmwareOTAStateEvent(_ data: Data) -> FirmwareOTAStateEvent? {
        guard data.count >= 4, data[0] == 1, data[1] == otaTypeState else { return nil }
        let payloadLength = Int(UInt16(littleEndianBytes: data[2..<4]))
        guard data.count >= 4 + payloadLength else { return nil }
        let payload = data.subdata(in: 4..<(4 + payloadLength))
        return try? JSONDecoder().decode(FirmwareOTAStateEvent.self, from: payload)
    }

    public static func uiStatePayload(state: String, text: String) -> Data {
        let payload = [
            "event": "ui_state",
            "state": state,
            "text": text
        ]
        return (try? JSONSerialization.data(withJSONObject: payload)) ?? Data()
    }

    public static func interactionModePayload(_ mode: InteractionMode) -> Data {
        let payload = [
            "event": "interaction_mode",
            "mode": mode.rawValue
        ]
        return (try? JSONSerialization.data(withJSONObject: payload)) ?? Data()
    }

    /// 网关目标机名上报帧（对齐 Windows BleProtocol::GatewayTargetInfoPayload，P1 切换器）：
    /// `{"event":"gateway_target_info","name":"<name>"}`。固件绑定到当前连接对端并存
    /// NVS；>23 字节拒收——截断在 GatewaySupport.targetInfoName 完成。
    public static func gatewayTargetInfoPayload(name: String) -> Data {
        let payload = [
            "event": "gateway_target_info",
            "name": name
        ]
        return (try? JSONSerialization.data(withJSONObject: payload)) ?? Data()
    }

    /// 网关按键路由设置帧（对齐 Windows BleProtocol::GatewayKeymapSetPayload，P1 隧道融合）：
    /// `{"event":"gateway_keymap_set","key":"<key>","route":"software"|"passthrough"}`。
    /// 固件应答 `gateway_keymap` 报告帧；路由表持久化在固件 NVS（全局，非按目标）。
    public static func gatewayKeymapSetPayload(key: String, route: String) -> Data {
        let payload = [
            "event": "gateway_keymap_set",
            "key": key,
            "route": route
        ]
        return (try? JSONSerialization.data(withJSONObject: payload)) ?? Data()
    }

    /// IMU 调试开关帧（对齐 Windows BleProtocol::ShowImuDebugPayload）：
    /// `{"event":"show_imu_debug","enabled":true|false}`，固件在屏幕上显示 IMU 调试信息。
    public static func showIMUDebugPayload(enabled: Bool) -> Data {
        let payload: [String: Any] = [
            "event": "show_imu_debug",
            "enabled": enabled
        ]
        return (try? JSONSerialization.data(withJSONObject: payload)) ?? Data()
    }

    /// IMU 唤醒灵敏度帧（对齐 Windows BleProtocol::ImuWakeSensitivityPayload）：
    /// `{"event":"imu_wake_sensitivity","threshold":<lsb>}`。
    public static func imuWakeSensitivityPayload(thresholdLsb: Int) -> Data {
        let payload: [String: Any] = [
            "event": "imu_wake_sensitivity",
            "threshold": thresholdLsb
        ]
        return (try? JSONSerialization.data(withJSONObject: payload)) ?? Data()
    }

    /// 敲击开关帧（对齐 Windows TapEnabledPayload）：`{"event":"tap_enabled","enabled":bool}`。
    public static func tapEnabledPayload(enabled: Bool) -> Data {
        let payload: [String: Any] = [
            "event": "tap_enabled",
            "enabled": enabled
        ]
        return (try? JSONSerialization.data(withJSONObject: payload)) ?? Data()
    }

    /// 敲击灵敏度帧（对齐 Windows TapSensitivityPayload）：
    /// `{"event":"tap_sensitivity","level":<1-10>}`。
    public static func tapSensitivityPayload(level: Int) -> Data {
        let payload: [String: Any] = [
            "event": "tap_sensitivity",
            "level": level
        ]
        return (try? JSONSerialization.data(withJSONObject: payload)) ?? Data()
    }

    /// 编码器录音灯颜色帧（对齐 Windows EncoderLedColorPayload）：
    /// `{"event":"encoder_led_color","color":"<8 色之一>"}`，固件持久化到 NVS。
    public static func encoderLedColorPayload(color: String) -> Data {
        let payload: [String: Any] = [
            "event": "encoder_led_color",
            "color": color
        ]
        return (try? JSONSerialization.data(withJSONObject: payload)) ?? Data()
    }

    /// 编码器录音门控帧（对齐 Windows EncoderRecordingGatePayload）：
    /// `{"event":"encoder_recording_gate","enabled":bool}`（press_action=recording → true）。
    public static func encoderRecordingGatePayload(enabled: Bool) -> Data {
        let payload: [String: Any] = [
            "event": "encoder_recording_gate",
            "enabled": enabled
        ]
        return (try? JSONSerialization.data(withJSONObject: payload)) ?? Data()
    }

    /// 主动请求电量上报（对齐 Windows BatteryStatusRequestPayload）：
    /// `{"event":"battery_status_request"}`。
    public static func batteryStatusRequestPayload() -> Data {
        let payload = ["event": "battery_status_request"]
        return (try? JSONSerialization.data(withJSONObject: payload)) ?? Data()
    }

    /// USB 供电自动关机开关（对齐 Windows UsbAutoOffPayload）：
    /// `{"event":"usb_auto_off","enabled":bool}`；固件回推 power_mgmt 事件确认。
    public static func usbAutoOffPayload(enabled: Bool) -> Data {
        let payload: [String: Any] = [
            "event": "usb_auto_off",
            "enabled": enabled
        ]
        return (try? JSONSerialization.data(withJSONObject: payload)) ?? Data()
    }

    /// power_log 命令帧（对齐 Windows PowerLog*Payload，经 control_rx 下发）。
    public static func powerLogClearPayload() -> Data {
        powerLogCommandPayload(["cmd": "clear"])
    }

    public static func powerLogTimeAnchorPayload(epoch: UInt32) -> Data {
        powerLogCommandPayload(["cmd": "time_anchor", "epoch": epoch])
    }

    public static func powerLogDumpPayload(offset: UInt32, max: UInt32) -> Data {
        powerLogCommandPayload(["cmd": "dump", "offset": offset, "max": max])
    }

    private static func powerLogCommandPayload(_ body: [String: Any]) -> Data {
        let payload: [String: Any] = ["power_log": body]
        return (try? JSONSerialization.data(withJSONObject: payload)) ?? Data()
    }

    /// 远程按键控制帧（对齐 Windows BleProtocol::RemoteButtonPayload）：
    /// `{"event":"remote_button_<down|up>","button":"primary","source":<src>,"request_id":N}`。
    /// 固件侧等价一次远程主键按下/松开，音频链路真实完整；不受编码器录音门控约束。
    public static func remoteButtonPayload(action: String, button: String, source: String, requestID: UInt32) -> Data {
        let payload: [String: Any] = [
            "event": "remote_button_\(action)",
            "button": button,
            "source": source,
            "request_id": requestID
        ]
        return (try? JSONSerialization.data(withJSONObject: payload)) ?? Data()
    }

    public static func otaBeginPayload(imageSize: UInt32, transferID: UInt32) -> Data {
        var data = Data([1, otaTypeBegin, 12, 0])
        data.appendLittleEndian(imageSize)
        data.appendLittleEndian(transferID)
        return data
    }

    public static func otaDataPayload(transferID: UInt32, offset: UInt32, chunk: Data) -> Data {
        var data = Data([1, otaTypeData, 12, 0])
        data.appendLittleEndian(transferID)
        data.appendLittleEndian(offset)
        data.append(chunk)
        return data
    }

    public static func otaEndPayload(transferID: UInt32, imageSize: UInt32) -> Data {
        var data = Data([1, otaTypeEnd, 12, 0])
        data.appendLittleEndian(transferID)
        data.appendLittleEndian(imageSize)
        return data
    }

    public static func otaAbortPayload(transferID: UInt32) -> Data {
        var data = Data([1, otaTypeAbort, 8, 0])
        data.appendLittleEndian(transferID)
        return data
    }

    // ---- 小米遥控器（ATVV）设备判定（对齐 Windows ble_protocol.cc）----

    /// 小米名称白名单判定（trim + 小写比较；中文名无大小写，仅 ASCII 受影响）。
    public static func isXiaomiRemoteName(_ name: String) -> Bool {
        let value = name.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        return value == "mi rc" || value == "xiaomi bluetooth remote 2 pro" ||
            value == "小米蓝牙语音遥控器" || value == "rc001" || value == "rc003"
    }

    /// 名称 → 设备类别：白名单或 `RC-`+4hex 为小米；`VS-`+4hex 为 StickS3。
    public static func deviceClass(forName name: String) -> DeviceClass? {
        if isXiaomiRemoteName(name) { return .xiaomiRemote2Pro }
        let value = name.trimmingCharacters(in: .whitespacesAndNewlines).uppercased()
        // 对齐 Windows IsHex4(value.substr(3, 4))：仅取前缀后前 4 个字符判定。
        // isASCII 门对齐 Windows C-locale isxdigit（仅 ASCII）：Swift isHexDigit
        // 会收 Unicode 全角 hex（如 １２３４），须排除。
        let suffix = value.dropFirst(3).prefix(4)
        let isHex4 = suffix.count == 4 && suffix.allSatisfy { $0.isASCII && $0.isHexDigit }
        if value.hasPrefix("RC-"), isHex4 { return .xiaomiRemote2Pro }
        if value.hasPrefix("VS-"), isHex4 { return .stickS3 }
        return nil
    }

    /// macOS 读不到蓝牙地址：由 CoreBluetooth 外设 UUID 字符串确定性派生
    /// `RC-XXXX`（SHA-256 前 2 字节大写 hex）。撞 ID 顺延由调用方处理。
    public static func rcDeviceID(fromPeripheralUUID uuidString: String) -> String {
        let digest = SHA256.hash(data: Data(uuidString.utf8))
        let hex = digest.prefix(2).map { String(format: "%02X", $0) }.joined()
        return "RC-" + hex
    }
}

private extension UInt16 {
    init(littleEndianBytes bytes: Data.SubSequence) {
        self = bytes.enumerated().reduce(0) { $0 | UInt16($1.element) << UInt16($1.offset * 8) }
    }
}

private extension UInt32 {
    init(littleEndianBytes bytes: Data.SubSequence) {
        self = bytes.enumerated().reduce(0) { $0 | UInt32($1.element) << UInt32($1.offset * 8) }
    }
}

private extension Data {
    mutating func appendLittleEndian(_ value: UInt32) {
        append(UInt8(value & 0xff))
        append(UInt8((value >> 8) & 0xff))
        append(UInt8((value >> 16) & 0xff))
        append(UInt8((value >> 24) & 0xff))
    }
}
