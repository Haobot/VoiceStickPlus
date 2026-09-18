# P0 — 僵尸会话自愈（无需用户干预）

> 上游：`Doc/Plan/xiaomi-gateway-followup-roadmap.md` §2-P0（§3.1 决策点 1：A/B/C 力度）、
> `Doc/Expe/ble-zombie-link-reboot-reconnect.md`（僵尸链路安定窗）、
> `Doc/Expe/xiaomi-gateway-voice-key-and-hid-passthrough-2026-09-18.md`（系统配对与 HOGP 前提）。
> 状态：2026-09-19 实施；力度分级由真机实测收敛（见 §3）。

## 1. 问题（本机日志取证，非推测）

设备重启 → LTK 轮换 / 加密上下文失效 ⇒ app 的 CCCD 订阅与写入可能**「假成功」**，
而设备侧从未登记（`state_sub=0`）⇒ `voice_ble_is_ready()` 恒假、录音被拒 ⇒ 语音静默失效。

`%LOCALAPPDATA%\VoiceStick\VoiceStickApp.log` 实证（2026-09-18）：

```
14:31:05 state subscribe VS-53A8 status=Success      ← 写入假成功（对照健康连接：同一步后 ~125ms 即收到 device_info）
14:31:06 audio subscribe VS-53A8 status=Success
14:31:06 connected VS-53A8 / conn_snapshot reason=connected paired=[VS-53A8(ready)]
14:32:42 heartbeat teardown VS-53A8 reason=no_rx_timeout silent_ms=96520   ← 96.5s 后才发现
14:55:20 zombie session VS-53A8: bond repair required
14:55:20 device disconnected VS-53A8; restarting scan for reconnection     ← 此后 8 小时零恢复
14:56:38 XiaomiKeymapHook: raw input device not remote: ...HID#...70041ddc53aa   ← 设备已被系统 HID 宿主连上（停广播）
```

两个独立缺陷：

| # | 缺陷 | 后果 |
|---|---|---|
| D1 | 置 `ready` 只看 CCCD 写返回 `Success`，没有「设备真的收到了」的正向证据 | 假成功被当成连接成功：UI 显示已连接、录音被拒（设备侧 `state_sub=0`），且要等 90s 心跳超时才判僵尸 |
| D2 | 判出僵尸后只 `StartScan()`，不排按地址主动直连 | 设备已被系统 HID 宿主连上而停止广播 ⇒ 扫描永远等不到 ⇒ 永久失联 |

**D2 说明「A 方案从未被真正执行过」**：僵尸路径压根没有发起过重连。

## 2. 判据：连接期活性证明

订阅的**任何入站 notify** 都算证据：固件的 `send_state_json` 受 `s_state_subscribed` 门控，
收到 state 通知即证明设备侧确实登记了订阅。两种来源：

- 固件在 `state_subscribed` 置位时立即下发 `device_info` + `encoder_status`
  （`voice_ble.c` 的 `BLE_GAP_EVENT_SUBSCRIBE`）—— 但实测这两帧是**本连接的第一个通知**，
  常被 Windows BTHLE 在 handler 接线完成前吞掉（本机日志约半数连接收不到，如 02:51 那次）。
- 因此补一发 `battery_status_request`（与 30s 心跳同一机制）主动索要回包：健康链路实测
  <1s 回 `battery_status`；僵尸链路一个字节都收不到。

检查点放在 audio 订阅完成之后、`ready = true` 之前：证据取 `session->last_rx_ms > 0`
（新会话初值为 0，未被任何写入污染）。取不到即判僵尸，**不再向协调器发布假「已连接」**，
交自愈梯度处理；超时 2.5s，远快于旧路径的 90s。

## 3. 自愈梯度（真机实测收敛后的力度分级）

| 级 | 动作 | 副作用 | 触发条件 |
|---|---|---|---|
| **A 轻量重连** | 只回收 WinRT 侧对象（`Close GattSession` + `BluetoothLEDevice`），按地址直连 | 无 —— 不动系统配对，**HOGP 按键直通不受影响** | 首次判僵尸 |
| **B 重置无线电** | `radio off/on` 清 controller 级加密上下文缓存 | 一次全链路中断（radio 灭约 5s）；**系统配对保留**，HID 宿主用同一把 LTK 自行恢复 | 同一故障期内 A 已试满 `kZombieLightAttemptsBeforeRepair`(=2) 次 |
| **末级 用户** | 保留现状，托盘气泡指引到系统蓝牙设置重配 | 打扰用户 | B 已用满 `kZombieMaxFullRepairsPerEpisode`(=1) 次仍失败 |

**为什么 B 不含 unpair（本计划最重要的实测结论）**：
2026-09-19 03:00 首版实现按旧「stale bond」套路做了 unpair + radio reset + PairAsync，
结果**语音自愈成功、按键直通被弄死**：`BTHPORT\Parameters\Devices\70041ddc53aa` 仍有密钥，
但 `Enum\BTHLE\Dev_70041ddc53aa` 节点与 HID 设备消失。事后单独复现 `PairAsync`：
只要设备被 app 连上（NimBLE 连上即 `stop_advertising()`），必然 `status=19 Failed`（连续 12 次）；
而配对对话框流程（扫描看到广播后再 `PairAsync`，同日 01:54 真机）能成功。
⇒ **自愈路径永不删 bond**；缺 bond 的重建交给独立看门狗，在「无会话 + 设备在广播」条件下做。

## 4. 组件与接口

- **纯逻辑（TDD）**：`BleProtocol::PlanZombieHeal(light_attempts, full_repairs, full_repair_allowed)`
  → `ZombieHealLevel{kLightReconnect, kFullRepair, kUserAction}`。
- **连接期活性证明**：`ConnectDeviceAsync` 内，audio 订阅后等 `kSessionLivenessTimeout`(2.5s)。
- **故障期记账**：`zombie_episodes_[address]`，一次故障期 `kZombieEpisodeWindow`(10min)；
  会话恢复即清零。
- **主动直连**：僵尸拆除后 `ScheduleZombieReconnect`（登记队列 + 延迟线程提前唤醒），
  不再只 `StartScan`（原本这是死路：设备已停广播）。
- **B 级**：`ConsumeZombieRepair` 标记在下次连接、打开设备后消费，执行 radio reset。
- **bond 看门狗（只补不删）**：`ProbeSessions` 每 10min/地址探测一次 `IsPaired()`；
  缺失时才拆会话 → 等设备重新广播 → `PairAsync` 重试 3 次 → 重连。每次运行最多 3 次。

## 5. 验收

| 项 | 口径 |
|---|---|
| 自愈 | 设备重启后**不改任何系统设置**，app 自行回到 `stage=ready` 且 `state notify` 恢复流动 |
| 稳定性 | 连续 ≥3 次重启无人工干预 |
| 不回归 | 自愈期间与之后 HOGP 按键直通仍可用（系统配对从未被删） |
| 单测 | `voicestick_windows_tests` 全过（含 `TestPlanZombieHeal` 边界断言） |

## 6. 非目标

- 不解决「LTK 为何轮换」——设备重启产生新 LTK 是 BLE 加密的固有行为。
- 不改固件：`state_sub=0` 是设备的正确行为，问题在主机侧的自愈能力。
- 不动扫描/广播策略（`voice_ble` 连上即停广播是既定设计，见 §8 交接文档更正）。
