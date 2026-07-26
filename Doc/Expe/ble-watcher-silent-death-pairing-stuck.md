# BLE 广告 watcher 静默失效：设备卡 Pairing、重启设备无效、重启 app 立愈

日期：2026-07-26。相关文件：`desktop/windows/src/ble_central_win.cc` / `.h`。

## 症状

设备屏幕停在 Pairing（VS-XXXX），反复重启 stick 无效；重启 Windows 端
VoiceStick.exe 立即恢复。

## 日志判据（先看日志，不要猜）

`%LOCALAPPDATA%\VoiceStick\VoiceStickApp.log`：

- **watcher 静默失效（本文）**：最后一条 `scan started` 之后长时间（小时级）
  **完全没有** `advertisement matched`，尽管设备一直在广播。重启 app 后
  `advertisement matched` / direct connect 立即出现。
- 对照——在途连接协程挂起（claim 泄漏）：会有 `advertisement matched` +
  `connecting` / `connect stage` 开头，但永远缺 `connected` / `connect failed`
  收尾。本次事故日志（57k 行）中 154 次 connect_begin 全部有收尾，该路径
  **零发生**，故排除。

## 根因

`BluetoothLEAdvertisementWatcher` 会静默失效：仍报告 Started 却不再投递任何
广告包（代码注释早已记录此现象，但此前只有系统休眠恢复
`RestartForResume()` 一条重建路径）。两个已确认的触发源：

1. **应用自己的 radio reset**：陈旧 bond 恢复路径调
   `TryResetBluetoothRadioAsync()` 关开蓝牙无线电，watcher 随之死亡；连接
   失败路径（`fail()`）只设 5s 退避、清 claim，**不重启扫描**——退避到期后
   再也没有广播进来。日志实证：00:03:20 radio reset → 00:04:13 connect
   failed → 之后 12 分钟零广播，00:16 重启 app 才恢复。
2. **系统侧原因**（驱动/无线电状态变化，无电源事件）：watcher 假活，
   应用对广播全盲。日志实证：01:41:04 断连后 scan started → 之后 6.6 小时
   零 `advertisement matched`（期间多次重启 stick，设备一直在广播），
   08:20 重启 app 立即恢复。

设备端行为是健全的（未连接永远广播，断连即重广播），所以「重启 stick 无效、
重启 app 立愈」精确对应「watcher 死了而无人重建」这一纯进程内状态。

## 修复（2026-07-26）

四层防线，都在 `ble_central_win.cc`：

1. **蓝牙无线电 `StateChanged` 事件订阅**（`InitRadioWatcherAsync()`，app
   启动时）：系统蓝牙开关切换（设置/操作中心）会杀死 watcher 与全部链路，
   无线电恢复（On）时立即 `RestartForResume()` 整体重建，秒级回连。应用
   自己的 radio reset 期间用 `self_radio_reset_` 标记跳过本处理（该路径
   已显式 StartScan，避免拆掉在途连接的 claim）。
2. **watcher `Stopped` 事件订阅**（`StartScan()`）：非正常停止
   （`Error != BluetoothError::Success`）记日志并经 UI 线程重建扫描；
   `StopScan()` 先反注册 token 再 `Stop()`，避免自停误触发。
3. **扫描静默看门狗**（`CheckScanHealth()`，心跳线程每 30s 调用）：有配对
   设备无 ready 会话、扫描在跑、却超过 `kScanSilenceTimeout`（60s）收不到
   **任何**广告包（`last_adv_received_ms_`，任意设备广告都算存活证明，
   `StartScan()` 成功时写基线）→ 判定 watcher 假活并 `StartScan()` 重建，
   `kScanWatchdogMinRestartInterval`（120s）节流。这是非无线电类失效
   （驱动异常等无事件场景）的兜底。
4. **radio reset 后无条件重建扫描**：两处 `TryResetBluetoothRadioAsync()`
   调用点（stale bond 与 Unreachable 恢复路径）之后都经 UI 线程
   `StartScan()`，无论 reset 与后续重连成败。

配套加固（针对「在途协程挂起 → claim 泄漏 → 自我封锁」这一同构隐患，
本次日志未发生但结构上等价）：

- audio CCCD 订阅补与 state 订阅同款的 `kSubscribeTimeout`（2.5s）竞速
  超时——此前 state 有超时 audio 没有，链路死在两次订阅之间即永久挂起。
- `connecting_addresses_` 从 set 改为 map（claim 时间戳），
  `CheckScanHealth()` 强制释放滞留超 `kConnectClaimTimeout`（120s，最坏
  正常连接实测 ~65s）的 claim 并记日志。
- claim 被拒（`try_claim_connect` 返回 false）时记限流日志（每地址 60s
  一条），消除「广播到了却无声无息」的诊断盲区。

## 经验

- watcher 静默失效的判据是「**零** `advertisement matched`」，而不是连接
  失败——失败至少有日志，假活什么都没有。
- 应用自己关开无线电（radio reset）和系统休眠一样会杀死 watcher；任何
  radio reset 之后都必须重建扫描，包括失败路径。
- 心跳探活（`ProbeSessions`）只看已就绪会话；「连接中」「扫描中」这两类
  状态需要各自独立的看门狗，不要假设已有机制能覆盖。
- 事件驱动优先于超时轮询：蓝牙开关切换有 `Radio.StateChanged` 事件可订阅，
  秒级恢复；超时看门狗（60s）只作无事件场景的兜底，不要拿它当主路径——
  用户实测「关开蓝牙后等 1~2 分钟才回连」即超时兜底作为主路径的体验。
