# macOS BLE OTA 吞吐 <1KB/s（Windows 同链路快）——chunkSize 固化于 MTU 协商前

- 日期：2026-10-02
- 相关文件：`desktop/macos/Sources/VoiceStickApp/BleCentral.swift`（`sendNextFirmwareUpdateFrame`）、`desktop/macos/Sources/VoiceStickCore/BleProtocol.swift`、对照 `desktop/windows/src/ble_central_win.cc:3185+`
- 症状：Mac 端固件 OTA 速度 <1KB/s（1.5MB 要 ~25 分钟）；Windows 端同一设备同一链路正常（KB/s~十KB/s 级）。

## 判据（下次快速识别）

- Mac OTA 慢但能完成 + 无 stalled 报错 ⇒ 先查 chunkSize 实际值（旧代码不打日志，需加）：`peripheral.maximumWriteValueLength(for: .withoutResponse)` 在 OTA 开始时取一次并固化进 session——**OTA 开始可能早于 ATT MTU 协商完成**，届时返回 20（MTU 23 假设）→ 每包 20B chunk（12B 帧头共 32B/包）→ 30ms 连接间隔下 ≈ 数百 B/s，与 <1KB/s 观测吻合。
- Windows 同位置取 `GattSession.MaxPduSize()`（WinRT 连接即协商，≥247）→ chunk 232B/包，天然快 10 倍+。

## 根因

1. **主根因**：chunkSize 一次性固化在 `beginFirmwareUpdate`，CoreBluetooth 的 MTU 协商是异步完成的，时序竞态使 Mac 永久 20B/包。
2. **次因（顺带补齐）**：Mac 无在途窗口流控——Windows 已定案「首确认 40KB / 稳态 24KB + 超窗 200ms 低节拍续发」（覆盖旧固件 32KB 进度回传间隔防互等死锁；防在途过大灌满对端控制器断链，2026-09-20 教训），Mac 完全靠 canSendWrite 背压，跨版本契约不对齐。

## 修复（2026-10-02，两端语义对齐）

1. chunkSize 逐轮惰性重取（MTU 协商完成后自动升到 `min(maxWrite-12, 244)`），变化时打一条日志（`OTA data chunk_size= max_write= window=`，对齐 Windows LogBleLine）。
2. 在途窗口下沉 `BleProtocol.otaMaxInFlightBytes(confirmedWritten:)`（Core，单测锁定 40KB/24KB）+ `otaDataHeaderLength=12` / `otaMaxChunkSize=244` 契约常量；超窗 200ms 低节拍续发（Timer，progress 到达即全速）；progress 事件推进 `confirmedWritten` 并触发续发；15s 看门狗兜底停确认。
3. 清理点：done/fail 均停节拍 Timer。

## 验证程度

- 单测 552/552（新增 6 条流控断言）。
- **真机吞吐验证待做**（设备在手，用本地文件 OTA 重刷当前版本测速；预期从 <1KB/s 升到 chunk 173B+/包 量级）。寄存器值/时序为记录时点结论，引用前以当前源码为准。
