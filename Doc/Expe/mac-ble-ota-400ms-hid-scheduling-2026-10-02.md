# macOS BLE OTA 580B/s——已配对 HID 设备被强制 400ms 省电调度（平台硬限制，定案）

- 日期：2026-10-02
- 相关：`desktop/macos/Sources/VoiceStickApp/BleCentral.swift`（OTA 发送/节拍/readRSSI workaround）、`firmware/components/voice_ble/voice_ble.c`（`voice_ble_request_fast_interval`）、对照 `desktop/windows/src/ble_central_win.cc`
- 关联：`Doc/Expe/mac-ble-ota-slow-throughput-2026-10-02.md`（第一层修复：chunkSize 惰性重取+流控，仍慢）

## 排查链（三层排除，全部实测）

1. ~~chunk 20B~~：修复后 `OTA begin max_write=244, chunk_size=232`（与 Windows 等大）——排除。
2. ~~发送突发灌满对端~~：固件串口自报 `write_max=0ms`（flash 写零压力）；改限深节拍 2 包/15ms 后依旧 580B/s——排除。
3. **真凶**：串口 `conn updated: interval=320 latency=4 timeout=1000`（=400ms）恒定；OTA 期间固件 `requested fast conn interval 7.5ms` → **`conn updated: status=571`（LL 拒绝）**，间隔不变；范围请求（7.5-30ms）同样被拒；app 侧周期 readRSSI 诱导亦无效。

## 根因定案

设备与 macOS 有 **HOGP 系统配对**（按键直通必需），该 ACL 由 bluetoothd 按 **HID profile 省电策略**调度（400ms + latency 4）；app 与系统共享同一条 ACL，**连接参数由 central（macOS）单方面决定**，外设任何参数请求（固定值/范围）都被拒，app 侧 readRSSI 也改变不了调度。**平台硬限制，无解**。Windows 的 HID over GATT 栈接受外设参数请求（或至少不锁 400ms），故快。语音功能不受影响的原因：音频靠 connection event 内多包 + 固件侧缓冲硬撑，用户无感知。

## 判据（下次快速识别）

- OTA 速度恒 ~0.6-1KB/s + 串口 `interval=320 latency=4` + 参数请求 `status=571` ⇒ 本案，走 USB。
- 若 `interval` 正常（≤30ms）但仍慢 ⇒ 查 chunk/窗口（第一层问题）。

## 处置（已落地）

- **体验**：更新窗口速度 <3KB/s 时提示「macOS 蓝牙限速（系统 HID 调度）；可改用 USB 数据线串口烧录（约 10 秒）」（中英双表）。
- **兜底链路**：USB 串口烧录（`esptool write_flash 0x10000 fw-app.bin`，实测定案 1.5MB/10s@921600 baud，Mac 直插 USB 即可）——VoiceStickFlash 是 Windows 独有，Mac 侧用 esptool 命令/浏览器烧录器，文档化即可（暂不做 Mac GUI 烧录工具，需求待产品定）。
- 遗留观察：若未来固件支持 2M PHY/DTM 或 Apple 开放 GATT 侧参数请求再回头；Bridge 模式（OTA 经 USB 转发）列为可选优化。
