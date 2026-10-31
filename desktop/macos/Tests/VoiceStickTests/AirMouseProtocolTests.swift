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

    // D6：分块预算 = maxWrite − 12B 帧头，**无下限兜底**。原 max(20, …) 在 ATT MTU
    // 未协商（maxWrite=20）时算出 20B chunk，总包 32B 超可写上限 → 固件必 bad_offset。
    checkEqual(BleProtocol.otaChunkSize(maxWrite: 20), 8, "D6: MTU 未协商时 chunk = 20-12（非 20）")
    checkEqual(BleProtocol.otaChunkSize(maxWrite: 12), 0, "D6: 放不下帧头 → 0（调用方报错）")
    checkEqual(BleProtocol.otaChunkSize(maxWrite: 11), 0, "D6: 负预算钳到 0")
    checkEqual(BleProtocol.otaChunkSize(maxWrite: 247), 235, "D6: 正常 = maxWrite-12")
    checkEqual(BleProtocol.otaChunkSize(maxWrite: 1000), 244, "D6: chunk 上限 244")
    // 自检：分块 + 帧头 永不超过写预算。
    checkEqual(BleProtocol.otaChunkSize(maxWrite: 20) + 12 <= 20, true, "D6: 包长不超预算")
}
