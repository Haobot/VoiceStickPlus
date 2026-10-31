# BLE OTA 对 v2.3.8 旧固件必死锁：app 流控窗口与固件进度回传间隔的跨版本耦合

- 日期：2026-09-26
- 相关文件：`desktop/windows/src/ble_central_win.cc`（`UpdateFirmwareAsync` 发送循环）、`desktop/windows/src/ble_protocol.h/.cc`（`OtaMaxInFlightBytes`）、`desktop/windows/tests/core_tests.cc`（`TestOtaMaxInFlightBytes`）、`firmware/components/voice_ble/voice_ble.c`（`OTA_PROGRESS_NOTIFY_BYTES`）
- 相关 commit：`983b7b7b`（修复）；流控原始约束见 `Doc/Expe/ble-zombie-self-heal-2026-09-19.md` 末节与 `Doc/Expe/gateway-p1-ota-session-2026-09-20.md`（互链）

## 症状

设备固件 v2.3.8（GitHub 无此 tag，`2fb5ff1b` merge 而来，进度回传间隔 32KB），经桌面端 v2.4.0 推送 GitHub 正式资产 `voicestick-firmware-sticks3-ota-2.4.0.bin`（1538240 字节）：弹窗「固件更新失败。Device stopped confirming OTA progress at 0 / 1538240 bytes」，两次重试均失败。

## 日志判据（app 日志 `%LOCALAPPDATA%\VoiceStick\VoiceStickApp.log`）

- `OTA begin ... size=1538240` 成功 → `OTA data chunk_size=232 max_pdu=247` → **约 1.2 秒内**出现 `OTA device progress stalled ... sent=24592 confirmed=0`。`sent=24592 = 24KB 窗口 + chunk 对齐`，**每次重试 sent 一模一样** = 流控窗口卡死特征（不是无线抖动——那会随机）。
- 设备串口（COM 115200）同时段：`OTA begin ... partition=ota_0` 成功后**零条** `OTA progress`，约 30s 后 `disconnected reason=533`（HCI 0x15，对端主动断）+ `OTA aborted after disconnect`。设备端零 progress 而非报错 = 数据其实收到了、只是没到回传阈值。
- 判据总结：**`sent` 恒等于窗口值 + `confirmed=0` + 设备端 begin 成功但零 progress ⇒ 流控互等死锁，不是链路问题**。

## 根因

2026-09-20 把固件进度回传间隔 `OTA_PROGRESS_NOTIFY_BYTES` 从 32KB 收到 8KB 时，桌面端把在途窗口配套定为 24KB（"必须大于回传间隔"约束按**当时**固件取值）。但旧固件仍是 32KB 间隔：

1. app 灌到 24KB（24592）就停，等设备确认；
2. v2.3.8 设备要攒满 **32KB** 才发第一条进度通知；
3. 双方互等 → 15s `kOtaConfirmStallTimeout` 兜底报错。

**鸡生蛋**：要升到 8KB 间隔的新固件必须先过 OTA，而 OTA 恰好被新旧流控参数差异堵死。2.4.0 发布时无人在旧固件上验证过 OTA（真机早已是 8KB 开发版）。

### 本机插曲（诊断时勿混淆）

- 19:37 有一次 1485600 字节的 OTA「成功」但设备版本未变——那次 bin 内容实为旧 v2.3.8（GitHub 无 v2.3.8/2.3.9 release，1485600 不属于任何发布资产，应为本地旧构建），把设备从 8KB 间隔的开发版 2.3.9 **降级**回 32KB 间隔的 2.3.8，随后才撞上本死锁。**桌面端 OTA 选文件时不校验 bin 内版本号（esp_app_desc_t 的 version 字段可读），用户传错 bin 无任何提示**——遗留改进项。

## 修复（commit 983b7b7b，两层）

均在 `UpdateFirmwareAsync` 发送循环：

1. **在途窗口自适应**：新增纯函数 `BleProtocol::OtaMaxInFlightBytes(confirmed)`——`confirmed==0`（从未收到确认）时 40KB（>32KB 旧间隔 + chunk 余量）；`confirmed>0` 时 24KB 常规节拍。TDD：先写 `TestOtaMaxInFlightBytes` 红灯再实现。
2. **超窗降速续发**：窗口等待分支不再纯停发，按 `kOtaWindowedWriteInterval`（200ms/块 ≈1.2KB/s）续发，让设备能攒到下一条回传阈值（32KB-24KB=8KB 缺口约 7s 补齐 < 15s stall 超时）；确认一到立即恢复全速。当初「窗口上限」防的是 48KB 高在途断链（一次性灌 ~200 无确认包），200ms 节拍的单位时间在途包极少，不触该条件。

## 验证（真机 VS-53A8，本地 v2.4.0 bin）

- 仅做第 1 层时：首条确认 32944 到达（死锁第一道解除），但第二条卡在 `sent=57536 confirmed=32944`——暴露 24KB<32KB 是**每档都死锁**，才补第 2 层。
- 两层齐后对 v2.3.8：1538240 全程传输 6 分钟零中断，`OTA end` → `OTA device done`，重启后 `device_info firmware_version: "2.4.0"`。
- 升级后（8KB 间隔）第二轮 OTA 全速 ~2 分钟完成——回归路径无劣化。CTest 全绿（含新增单测）。
- 中途一次失败 `OTA write timed out offset=131312` 是用户同时密集语音抢占 BLE 无线电所致（单块写 5s 超时），非流控问题；OTA 失败无损可重试。

## 长期技术记忆

- **app 与固件的流控参数是跨版本契约**：改任何一侧的回传间隔/窗口，必须检查「对最老仍在服役固件」的兼容性，不能只按当前配对调优。发布前在**最旧服役固件**上真机过一遍 OTA。
- 死锁排查口诀：先看 `sent` 是否恒等于窗口值——是则流控互等（纯逻辑 bug），随机值才是无线问题。
- Windows 端 `OTA write timed out`（5s 单块）在 BLE 无线电被本机其他业务抢占时可复现；对 write_without_response 无法重发同一 offset（固件 `offset != written` 严格校验 + 无响应通道），只能整体重试。
- 用户/并行会话可能同时操作设备：自动化 OTA 验证前确认设备空闲（日志无 button/encoder 活动），否则把人为干扰误判为 bug。

## 遗留 / 观察项

- 桌面端 OTA 不校验所选 bin 的版本号/硬件匹配（`esp_app_desc_t` 可读）：传错 bin 会静默降级/升级，建议选文件时解析并展示版本。
- 固件 manifest 双源在国内均不可靠：主源 `dl.davenger.cloud` 未上线，GitHub release 下载 302 后被墙（curl 实测 HTTP=302 size=0），曾拉到旧缓存 `version=2.3.7`。等 COS 上线联调（用户主导）。
- `FindPythonExe` 开发路径 `build-x64/flash_payload/python/python.exe` 需手工跑 `scripts/prepare_flash_payload.ps1 -OutputDir ...` 准备；脚本默认 python.org 直连在 PS5.1 下会 TLS 握手失败，可先 `curl` 下载 zip 到 `%TEMP%\voicestick-python-embed-<ver>.zip` 走缓存。
