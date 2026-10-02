import Foundation
import VoiceStickCore

// OTA 流控与帧预算单测（对齐 Windows BleProtocol::OtaMaxInFlightBytes 定案参数；
// 窗口数值是 app×固件跨版本契约，改动须在两端与最旧服役固件上同步验证）。

func runOtaFlowControlTests() {
    // 首窗 40KB 覆盖 v2.3.8 及更早固件的 32KB 进度回传间隔（防互等死锁）；
    // 确认流动后收紧 24KB（防持续在途过大灌满对端控制器断链）。
    checkEqual(BleProtocol.otaMaxInFlightBytes(confirmedWritten: 0), 40 * 1024,
               "OTA 首确认前窗口 40KB")
    checkEqual(BleProtocol.otaMaxInFlightBytes(confirmedWritten: 1), 24 * 1024,
               "OTA 确认流动后窗口 24KB")
    checkEqual(BleProtocol.otaMaxInFlightBytes(confirmedWritten: 100 * 1024), 24 * 1024,
               "OTA 稳态窗口恒 24KB")
    // 帧头预算与 chunk 上限（对齐 Windows min(max_pdu-15, 244)：15=12B 帧头+3B ATT 头）。
    checkEqual(BleProtocol.otaDataHeaderLength, 12, "OTA 数据帧头 12B")
    checkEqual(BleProtocol.otaMaxChunkSize, 244, "OTA 单包 chunk 上限 244B")
    // 数据帧构造：总包长 = 12 + chunk（chunk 预算自检用）。
    let payload = BleProtocol.otaDataPayload(transferID: 7, offset: 100, chunk: Data(repeating: 0xAB, count: 244))
    checkEqual(payload.count, 12 + 244, "OTA 数据帧总长 = 帧头 + chunk")
}
