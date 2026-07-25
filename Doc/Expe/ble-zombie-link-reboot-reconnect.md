# BLE 僵尸链路：设备快速重启后回连慢（>7s Pairing）的根因与修复

日期：2026-07-25。相关文件：`desktop/windows/src/ble_central_win.cc`。

## 症状

设备手动重启（或 OTA 重启）后，屏幕停在 Pairing 超过 7 秒才连上。
深睡唤醒不受影响（几分钟的睡眠期间僵尸链路早已死亡）。

## 根因：Windows 僵尸链路

设备重启 → Windows 仍持有旧 BLE 链路（对端静默消失，WinRT 不投递断连事件）
→ 设备重新广播 → app 拆掉陈旧会话并立即重连 → **第一次连接挂在僵尸链路上**：

1. `link-layer connected after 0ms polls=0`：OS 认为"已连接"，根本没建立新链路；
2. 服务/特征发现走本地缓存（无空口流量）全部"成功"；
3. 第一个真正的空口操作（state CCCD 订阅写入）发出后无人应答，挂起；
4. ~3.5-4.0s 后旧链路监督超时，OS 宣告 `disconnected` + `GattServicesChanged`，
   订阅被取消（`0x800704C7 操作已被用户取消`），连接失败；
5. 5 秒失败退避（`connect_cooldown_until_`）；
6. 第二次尝试建立真实新链路，~1.5-2.4s 成功。

全程 ~10.8s（14:56:57 与 15:06:50 两次真机复现，模式完全一致）。

关键判据：安定窗前两次失败尝试都是 `polls=0` 秒连（僵尸），成功的那次
`polls=1~9`（真实建链）。

## 修复：僵尸链路安定窗

`HandleAdvertisement` 拆陈旧会话时，若旧会话"刚刚还活跃"
（`last_rx_ms` 在 45s 内，覆盖心跳 30s 间隔的存活会话），给该地址设 4.5s
`reconnect_settle_until_` 安定窗，`try_claim_connect` 在窗内拒绝连接，
等 OS 宣告旧链路死亡（实测 3.5-4.0s）后由下一个广播包触发连接。

- 快速重启：~10.8s → 预计 ~6.5-7s（4.5s 安定 + ~1s 等广播 + ~1.5s 连接）。
- 深睡唤醒：旧会话已安静数分钟（last_rx 超阈值），不进窗口，保持原有快速路径。

## 经验

- Windows 对静默消失的 BLE 对端不投递断连事件；`BluetoothLEDevice.Close()`
  只是释放本进程句柄，OS 级链路的死亡由监督超时决定（本机实测 3.5-4s）。
- 判断"连的是不是僵尸链路"看 `wait_connected` 的 polls：0 轮秒连 = 僵尸，
  有轮询 = 真实建链。
- 排查此类问题先看 `%LOCALAPPDATA%\VoiceStick\VoiceStickApp.log` 的
  connect stage 全链路时间戳，不要猜。
