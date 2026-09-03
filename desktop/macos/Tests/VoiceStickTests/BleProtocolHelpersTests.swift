import Foundation
import VoiceStickCore

/// BleProtocol 设备判定辅助函数确定性（对齐 Windows IsXiaomiRemoteName/DeviceClassFromName）。
/// 取自旧 XCTest DeviceIDAndConfigTests 中不依赖 AppConfig 的三个用例；
/// normalizedDeviceID/paired_devices CSV/xiaomi TOML 用例属 app 模块，本 runner 不覆盖。
func runBleProtocolHelpersTests() {
    // ---- testIsXiaomiRemoteName ----
    check(BleProtocol.isXiaomiRemoteName("MI RC"), "IsXiaomiRemoteName.miRcUpper")
    check(BleProtocol.isXiaomiRemoteName(" mi rc "), "IsXiaomiRemoteName.trimmed")
    check(BleProtocol.isXiaomiRemoteName("Xiaomi Bluetooth Remote 2 Pro"), "IsXiaomiRemoteName.fullName")
    check(BleProtocol.isXiaomiRemoteName("小米蓝牙语音遥控器"), "IsXiaomiRemoteName.chinese")
    check(BleProtocol.isXiaomiRemoteName("RC001"), "IsXiaomiRemoteName.rc001")
    check(BleProtocol.isXiaomiRemoteName("rc003"), "IsXiaomiRemoteName.rc003Lower")
    check(!BleProtocol.isXiaomiRemoteName("VS-C3D8"), "IsXiaomiRemoteName.stickRejected")
    check(!BleProtocol.isXiaomiRemoteName(""), "IsXiaomiRemoteName.emptyRejected")
    check(!BleProtocol.isXiaomiRemoteName("MI RC2"), "IsXiaomiRemoteName.miRc2Rejected")

    // ---- testDeviceClassForName ----
    checkEqual(BleProtocol.deviceClass(forName: "MI RC"), .xiaomiRemote2Pro, "DeviceClass.miRc")
    checkEqual(BleProtocol.deviceClass(forName: "RC-3A7F"), .xiaomiRemote2Pro, "DeviceClass.rcHex")
    checkEqual(BleProtocol.deviceClass(forName: "VS-C3D8"), .stickS3, "DeviceClass.vsHex")
    checkNil(BleProtocol.deviceClass(forName: "Random Speaker"), "DeviceClass.randomNil")
    checkNil(BleProtocol.deviceClass(forName: "RC-123"), "DeviceClass.shortSuffixNil")
    // 全角 hex 字符不得误判（对齐 Windows C-locale isxdigit 仅 ASCII；Swift
    // Character.isHexDigit 会收 Unicode 全角，如 １２３４/ＡＢＣＤ）。
    checkNil(BleProtocol.deviceClass(forName: "RC-１２３４"), "DeviceClass.fullwidthDigitNil")
    checkNil(BleProtocol.deviceClass(forName: "VS-ＡＢＣＤ"), "DeviceClass.fullwidthLetterNil")
    checkNil(BleProtocol.deviceClass(forName: "RC-12３4"), "DeviceClass.mixedFullwidthNil")

    // ---- testRCDeviceIDDeterministic ----
    // SHA-256 前 2 字节大写 hex：同输入恒同输出（此处 pin 住具体值防漂移）。
    let id = BleProtocol.rcDeviceID(fromPeripheralUUID: "E621E1F8-C36B-445A-8C36-4A468E8F34A1")
    checkEqual(id, "RC-E7F3", "RCDeviceID.pinned")
    checkEqual(id, BleProtocol.rcDeviceID(fromPeripheralUUID: "E621E1F8-C36B-445A-8C36-4A468E8F34A1"),
               "RCDeviceID.deterministic")
    check(id != BleProtocol.rcDeviceID(fromPeripheralUUID: "9D6F1C2A-0000-4000-8000-112233445566"),
          "RCDeviceID.distinct")
}
