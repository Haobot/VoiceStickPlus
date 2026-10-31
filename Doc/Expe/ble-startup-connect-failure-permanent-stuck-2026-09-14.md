# BLE 启动直连失败后永久卡死：重启后已配对小米遥控器停在「正在连接」

日期：2026-09-14。相关文件：`desktop/windows/src/ble_central_win.cc`、
`desktop/windows/src/ble_protocol.cc/.h`。修复提交：`66796df3`。

## 症状

电脑重启后，VoiceStick（开机自启）对已配对的小米遥控器 RC-6459 发起启动
直连失败，此后 BLE 模块**静默 5 小时**（11:38 失败 → 16:16 用户手动忘记+
重配才恢复）。期间遥控器实际已被 Windows 系统连上（设备管理器显示「已连接」），
但应用 UI 永远停在「正在连接...」。

失败会话日志特征（与成功路径对比即定位）：

```text
wait_connected_done t=4516ms polls=40 status=disconnected   ← 4.3s 链路从未建立
GattSession created+maintained max_pdu_size=23              ← MTU 未协商（链路死的旁证）
service discovery attempt 1 mode=cached status=Success      ← 缓存命中，非真实链路
connect failed RC-6459 reason=atvv control subscribe timeout after 2500ms
（此后再无任何 BLE 日志）
```

对照组（同日手动重配成功）：`link-layer connected after 0ms` +
`max_pdu_size=247`，4.6s ready——链路活着时直连代码本身没问题。

## 根因：失败后零重试 × 小米不广播的广播盲区

两层叠加：

1. **触发**：开机自启启动太早，重启后蓝牙栈/系统 HID 重连尚未就绪，
   `wait_connected` 4.3s 窗口内链路未建立；cached 模式的服务/特征发现
   走本地缓存「假成功」，直到写 CCCD 需要真实链路才暴露，2.5s 超时失败。
   VoiceStick 放弃后几分钟，系统把遥控器 HID 连上了。
2. **死锁**：`fail` lambda 只设 5s 冷却 + 上报 `on_connection_error`（UI
   仅弹配对错误），无任何重试调度。仅有的两条自动重连路径全部失效：
   - 扫描路径（HandleAdvertisement）：**小米遥控器被系统 HID 连上后停止
     广播**，扫描永远等不到它（该盲区在代码注释里早有记载）；
   - 主动重连队列 `pending_proactive_reconnects_`（心跳 60s 按地址直连）：
     **唯一入队点在 HandleAdvertisement 的僵尸拆链分支**——要先收到广播
     才会入队，启动直连失败路径与它无缘。

StickS3 不受影响：固件未连接时持续广播，扫描路径可兜底。

## 修复：失败即入队心跳主动重连

- 新增纯函数 `BleProtocol::PlanReconnectAfterConnectFailure`（决策抽 core
  便于单测）：取消（`kConnectFailureReasonCancelled`）与已忘记不重试；
  僵尸免退避窗口立即到期；常规失败按 `kConnectFailureCooldown`（5s，
  原写死值抽常量）延迟。
- `ConnectDeviceAsync` 的 `fail` lambda 按决策入队
  `pending_proactive_reconnects_`；心跳（30s）到期检查仍配对/未连接/
  未在连接中才发起，成功或忘记设备后自动清项。
- 修复后预期行为：重启后首轮直连失败 → 5s 后心跳重试 → 遥控器被系统
  HID 连上后 FromBluetoothAddressAsync 复用现有链路秒连（对照组已证明）。

## 经验

- **重连机制要以「失败路径」为源头审计，不能只覆盖「断连路径」**：本次
  死锁的本质是 `pending_proactive_reconnects_` 只有一个广播触发的入队口，
  而「从未连上」的设备恰恰不会广播。
- **cached 模式的 GATT 发现成功不是链路证据**：`max_pdu_size=23` +
  `status=disconnected` 才是链路真相；判连接健康先看 MTU 有没有协商上去。
- 小米遥控器生命周期：广播（可被扫描发现）→ 被系统连上 HID（停止广播，
  只能按地址直连）→ 断开（恢复广播）。任何「等广告」的恢复策略对它都有
  天然盲区，心跳按地址直连是唯一可靠兜底。
- TDD 落点：WinRT 强依赖的 BleCentralWin 无法单测，把「失败后是否重试」
  的守卫矩阵抽成纯函数进 core（`ble_protocol`），单测覆盖取消优先级、
  忘记拦截、退避/免退避四象限。

## 验证与遗留

- 单测 `TestPlanReconnectAfterConnectFailure` 全绿；全量构建 + CTest
  （单测 46.9s + 集成 114.3s）无回归；重启应用正常直连 RC-6459（1.3s
  ready）。
- **重启场景真机验收待下次电脑重启**：预期日志序列为
  `connect failed ... [proactive reconnect queued in 5000ms]` →
  `proactive reconnect ... (heartbeat fallback: ...)` → `connected`。
