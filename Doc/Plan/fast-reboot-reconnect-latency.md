# 快速重启回连时长压缩

## 背景与现状

设备快速重启（如烧录后短按复位）后，Windows 端回连约 6.7s。上一轮回连优化
（`Doc/Expe/ble-zombie-link-reboot-reconnect.md`）已定性根因为 Windows 僵尸链路：
设备秒级重启后 OS 仍持有旧链路，直接连接会挂在僵尸链路上（首个 ATT 订阅挂起
~3.5-4s，直到 OS 监督超时宣告断连），现行方案是在 `HandleAdvertisement` 判出
"秒级前还活跃的陈旧会话"时设 4.5s 安定窗（`kReconnectSettleDelay`），等 OS 拆完
旧链路再连。

当前 ~6.7s 的构成：

| 阶段 | 耗时 | 说明 |
|---|---|---|
| 固件 boot → 开始 fast 广播 | 估 0.7-1.2s | 未实测，`voice_ble_init` 已提前到 `ui_status_init` 之前与屏幕初始化并行（`main.c:2144`） |
| 广播被发现 | <0.2s | fast adv 20-30ms，Windows 连续扫描 |
| 僵尸链路安定窗 | 4.5s | 最大头，本方案主攻 |
| 连接流程 | ~1-1.5s | open_device → 100ms settle → GattSession → 轮询 link → cached 服务发现 → state/audio 两次订阅 |

## 关键观察

安定窗 4.5s 的依据是"被动等 OS 宣告僵尸死亡需 3.5-4s"。但现行代码在
`HandleAdvertisement` 判出僵尸时已**主动** `Close()` gatt_session 与 ble_device
（`ble_central_win.cc:807` → `HandleDeviceDisconnected` → `:1922/:1931`），栈会立即
发 LL_TERMINATE 拆链——僵尸实际死亡时间很可能远小于 4s。安定窗因此有压缩空间。

风险：若安定窗调太短、撞上尚未拆完的僵尸，连接会退回"订阅挂 3.5s + 5s 退避"的
~10s 老路。因此缩短安定窗必须配应用层超时与快速重试兜底。

## 设计（用户已批准，激进全套的修正版）

### 1. 安定窗缩短（Windows）

`kReconnectSettleDelay` 4500ms → **1500ms**（初始值，真机实测后定终值）。

### 2. 首订阅应用层超时 + 僵尸场景免退避重试（Windows）

- state 订阅是链上第一个真正走空口的 ATT 操作（cached 服务发现可能命中 OS 缓存
  不走空口）。给它加 `when_any(op, WaitMs(2500))` 超时：超时即 `Cancel()` 并走失败
  路径。正常订阅几十 ms 完成，超时只在撞僵尸时触发。
- 对"经安定窗路径而来"的连接打 zombie_suspect 标记：其连接失败时**不设 5s 退避**，
  下一条广播（20-30ms 一条）立即触发重连。非僵尸路径的 5s 退避保持不变。
- 兜底效果：即使安定窗太短撞僵尸，总耗时 ≈ 1.5s（安定窗）+ 2.5s（超时）+
  ~1.3s（重连）≈ **5.3s 封顶**，不回退到 10s+。

### 3. 固件 boot→广播实测，按需提速（firmware）

先测量后动手，不预设改法：串口日志 `voice_ble.c:691` 的
`advertising as ... ts=` 对比 boot 首行日志 ts，得 boot→adv 耗时。
若 <1s 不动固件；若明显偏大，再查 `init_power_management` / `stick_s3_board_init`
中的延时与 bootloader 配置。

### 4. 明确剔除项：订阅并行化

初版"激进全套"描述含订阅并行化，代码核实后剔除：`ble_central_win.cc:1517-1521`
注释明确记录 state 必须先订阅，否则 Windows 把 device_info 推迟 ~1s；两条订阅间
还有刻意的 `kDeviceInfoSettleDelay`。并行会破坏该实测换来的约束。

## 预期与边界

- 回连 ~6.7s → **3.5-4s**；固件 boot 若可再压则接近 ~3s。
- 下限由 Windows 建链本身（~1-1.5s，应用层不可控）与固件 boot 决定。微信输入法等
  系统级方案的"快"来自常驻连接不断开，本方案是每次真重连，不以其为对标。

## 验证

- 真机快速重启 5-10 次，从桌面端日志看 `reconnect settle` →
  `link-layer connected polls` → 首订阅耗时 → `connected` 完整链路，统计回连总时长
  与一次成功率（成功判据沿用：成功尝试 polls≥1；失败特征 polls=0）。
- 撞僵尸兜底路径至少人为触发/观察一次（若自然出现）。
- `ctest -R voicestick_windows_tests` 保持全绿。
