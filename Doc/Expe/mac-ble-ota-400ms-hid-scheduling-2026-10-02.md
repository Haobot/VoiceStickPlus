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

## 追加节（2026-10-11/12）：「无解」定案被推翻——范围请求下限 15ms 即可被 macOS 接受

> 本节修正上文「平台硬限制，无解」的结论。旧结论保留如上（其 HOGP 配对调度观察仍
> 有效），但「外设参数请求一律被拒」不成立。相关 commit `fb09bec`，完整根因链见
> `Doc/Expe/voice-input-latency-root-cause-chain-2026-10-12.md`。

- **新证据**（macOS 27.0.1，设备 VS-53A8 网关模式，语音录音场景）：范围请求
  `itvl_min=6..24`（7.5-30ms）被拒——但拒绝方式是 `ble_gap_update_params` 同步返回
  **rc=530（= BLE_HS_ERR_HCI_BASE 0x200 + HCI 0x12 Unsupported Remote Feature）**；
  而**下限提到 12（15ms）后请求被接受**，串口实录 `requested fast conn interval` 后
  `conn updated: status=0 interval=12 latency=0 timeout=200`，音频通知从 ~2.4fps 回到
  **26fps 实时**，与 Windows 无异。
- **判据更新**：`fast interval request failed rc=530` = 请求范围含 Apple 不接受的
  <15ms 间隔（Apple 栈按自身下限校验整个范围，不是选档）；`conn updated: status=571`
  （本案旧判据）= LL 拒绝，两者都表现为间隔不动，但 rc=530 是**可修的参数问题**。
  间隔恒定 `interval=320` 且音频/OTA 慢时，先看请求 rc 再下「平台限制」结论。
- **与旧结论的差异假设**：旧案设备与 macOS 存在 **HOGP 系统配对**（bluetoothd 按 HID
  profile 调度 ACL），本案 app 链路无系统级 HID 配对——不排除 HID 调度场景下仍被锁
  400ms。**两案并存的可操作结论**：先试 15ms 下限的范围请求（廉价、一行改动），
  成了就赚到；被 `status=571`/`rc=530` 双拒再按平台限制走 USB 兜底。
- 附带修订（`voice_ble.c` `voice_ble_request_fast_interval`）：`min_ce_len/max_ce_len`
  8/8 归零（排除其作为拒绝因素）；请求幂等化 + CONN_UPDATE 授予慢参数自愈重申
  （每连接 5 次）+ 录音中 1.5s 周期重申。上文「app 侧 readRSSI 诱导无效」结论不变。
