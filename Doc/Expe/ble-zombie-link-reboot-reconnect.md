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

## 二轮优化（2026-07-25，安定窗 1.5s + 订阅超时 + 免退避重试）

设计：`Doc/Plan/fast-reboot-reconnect-latency.md`；
计划：`Doc/Plan/fast-reboot-reconnect-latency-impl.md`。
提交：`54d10c6`（安定窗 4.5s→1.5s + 免退避）、`e0d9e8d`（打标移到 cooldown
检查之后）、`7c06db6`（state 订阅 2.5s 超时）、`e1cc940`（免退避扩为
15s 窗内最多 3 次）。

核心认知修正：4.5s 安定窗的依据是"被动等 OS 宣告僵尸死亡需 3.5-4s"，但
`HandleAdvertisement` 判出僵尸时已主动 `Close()` gatt_session/ble_device，
栈会立即发 LL_TERMINATE，僵尸死亡远快于被动等待——安定窗可缩到 1.5s。
配套两道保险：`kSubscribeTimeout{2500}` 给链上首个 ATT 操作（state 订阅）
加应用层超时（撞未死僵尸提前取消，不再空挂 3.5-4s）；安定窗放行的连接打
zombie_suspect 标记，失败时 15s 窗内最多 3 次免 5s 退避立即重试。

真机验证（2026-07-25，7 个正常节奏样本，间隔 ≥30s）：

- 全部一次成功（polls≥1 真实链路，无订阅超时）；广告→已连接 4.6-6.1s，
  中位 5.4s（一轮优化前 ~6.7s，最初 ~10.8s）。
- 时长构成（硬成本，继续压缩空间有限）：固件 boot→广播 ~1s（日志反推，
  串口实测因 USB 重枚举丢数据未成功，但估计可信）+ 安定窗 1.5s +
  扫描/认领 ~0.5s + Windows 建链与服务发现 ~2s（`GattServicesChanged` 每次
  连接都会使缓存失效，cached 发现实际要走空口 ~760ms）。

连按风暴（秒级连续重启）压测：

- 每次首试撞僵尸（polls=0）→ 2.5s 超时 + `[zombie-suspect: no cooldown,
  immediate retry #N]` → 免退避重试 → 第 2（偶尔第 3）次成功，~6.5-7.5s 自愈。
- 加固前（单次免退避）出现过一个双僵尸病理案例：第 2 次重试也撞垂死链路，
  标记已消费落回 5s 退避，一轮 23s——这正是 15s/3 次加固的动机。
- 注意：`polls=0` 不是僵尸的唯一形态，观察到过 polls=1（OS 报告 Connected）
  但 ATT 仍挂起的"垂死链路"，订阅超时对两种形态都兜底。

经验：

- `when_any` 竞速超时：cppwinrt 的 `when_any` 用 static_assert 强制所有分支
  同类型，`IAsyncOperation<T>` 与 `IAsyncAction` 混搭编不过，需把操作包一层
  返回 `IAsyncAction` 的 IILE（内部 `try { co_await op; } catch (...) {}`
  吞异常——必须吞，`when_any` 以 fire_and_forget 等输家分支，异常逃逸会
  `std::terminate`）；结果判定仍以原 op 的 `Status()/GetResults()` 为准。
- 免退避必须**有界**（次数+时间双上限）：无限免退避会让持续失败的设备
  tight-loop；打标时间不能在重试间刷新，否则计数约束失效。
- 设备快速重启的串口日志采集不可靠：EN 复位导致 USB JTAG 重枚举，
  重枚举间隙（~1-2s）内打印的 boot 日志丢失；pyserial 句柄在重枚举后
  读 0 字节需重开。boot→adv 耗时可从主机日志反推（断连事件→重新广播）。
