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

    public init(event: String, button: String?, sessionID: UInt32?, durationMs: UInt32?,
                hardware: String?, firmwareVersion: String?, buttons: [String]?, uiStates: [String]?) {
        self.event = event
        self.button = button
        self.sessionID = sessionID
        self.durationMs = durationMs
        self.hardware = hardware
        self.firmwareVersion = firmwareVersion
        self.buttons = buttons
        self.uiStates = uiStates
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
        return try? JSONDecoder().decode(StateEvent.self, from: payload)
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
