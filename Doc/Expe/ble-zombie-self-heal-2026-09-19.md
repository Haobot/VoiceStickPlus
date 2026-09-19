# BLE 僵尸会话自愈：订阅「假成功」的判据，以及自愈路径绝不可 unpair

> 日期：2026-09-19　相关 commit：`1557276e`（P0 实现）
> 相关文件：`desktop/windows/src/ble_central_win.cc`、`desktop/windows/src/ble_protocol.{h,cc}`、
> `desktop/windows/tests/core_tests.cc`；
> 方案：`Doc/Plan/xiaomi-gateway-p0-zombie-self-heal.md`；
> 源头需求：`Doc/Plan/xiaomi-gateway-followup-roadmap.md` §2-P0
> 同主题旧文：`Doc/Expe/ble-zombie-link-reboot-reconnect.md`（僵尸链路与快速重启回连）、
> `Doc/Expe/xiaomi-gateway-voice-key-and-hid-passthrough-2026-09-18.md`（系统配对与 HOGP）
> 设备：VS-53A8（COM19，固件 2.3.9）；日志：`%LOCALAPPDATA%\VoiceStick\VoiceStickApp.log`

## 症状

设备（重启 / 烧录 / 崩溃）之后，app 显示已连接但**语音静默失效**：按语音键无文字上屏，
且要等很久（甚至永远）才恢复。旧实现下最终表现为「必须由用户到 Windows 蓝牙设置
删除并重新添加设备」。

## 日志判据（下次快速识别）

| 现象 | 判据 |
|---|---|
| 订阅**假成功** | `state subscribe VS-XXXX status=Success` 之后**长时间零入站 notify**。健康连接必然伴随 `state notify`（`device_info` 或回包 `battery_status`）。直接对照同一份日志里健康连接的时间差：订阅完成到首个 notify 通常 <1s |
| 设备侧真的没登记订阅 | 设备串口 `send_state_json gated: state_sub=0`（本机 USB 运行时日志不稳，以主机侧零入站反推） |
| 发现了僵尸但救不回来 | 只有 `device disconnected …; restarting scan for reconnection` 而**没有** `proactive reconnect` ⇒ 死路（设备被系统 HID 宿主连上、停广播，扫描永远等不到） |
| 设备已被系统 HID 宿主连上 | 日志出现 `XiaomiKeymapHook: raw input device not remote: …HID#{00001812-…}_<地址>…` |
| HOGP 按键直通已死 | `BTHPORT\Parameters\Devices\<地址>` 有密钥而 `Enum\BTHLE\Dev_<地址>` 无节点（同 §1.10 判据） |
| 新实现的自愈轨迹 | `zombie session … heal level=light-reconnect(A)` / `zombie heal B: radio reset …` / `zombie reconnect queued … (heartbeat fallback + early wake)` / `zombie heal B scheduled …` |

## 根因（两个独立缺陷）

### A. 判「连接成功」只看 CCCD 写返回 Success —— 假成功被当成真连接

加密上下文陈旧（设备重启致 LTK 轮换、WinRT 缓存未失效）时，`WriteClientCharacteristic
ConfigurationDescriptorAsync` 返回 Success 而设备侧什么都没收到。旧代码据此置
`ready = true`、向协调器发布「已连接」，于是 UI 显示正常、录音却必被拒（固件
`voice_ble_is_ready()` 要求 state+audio 均已订阅），且要等 **90s 心跳超时**才发现。
2026-09-18 真机实证：`14:31:05 state subscribe Success` → `14:31:06 connected` →
`14:32:42 heartbeat teardown reason=no_rx_timeout silent_ms=96520`。

### B. 判出僵尸后只重扫 —— 而设备已经不广播了

`ProbeSessions` 判僵尸后调 `HandleDeviceDisconnected` → 只 `StartScan()`。
但固件连接成功即 `stop_advertising()`，而设备此时多已被系统 HID 宿主连上
（`Enum\BTHLE` 节点存在时 Windows 会自动连），**扫描永远等不到**。
实测 14:55:20 拆除后连续 **8 小时**零恢复，直到进程重启。

> 这条也是「A 方案（轻量重连）无效」这一旧判断的证伪：僵尸路径压根没发起过重连，
> 谈不上验证过轻量重连的成败。

## 修复

### 1. 连接期活性证明（判据从「写入返回」改为「设备有回音」）

audio 订阅完成后、`ready = true` 之前，等 `kSessionLivenessTimeout{2500}` 内的
**首个入站 notify**；证据取 `session->last_rx_ms > 0`（新会话初值 0，未被任何写入污染）。
取不到即判僵尸，`fail()` 走自愈梯度，**不再向协调器发布假「已连接」**。

两个通知来源：

- 固件在 `state_subscribed` 置位时立即下发 `device_info` + `encoder_status`
  （`voice_ble.c` 的 `BLE_GAP_EVENT_SUBSCRIBE`）——**但这两帧是本连接的第一个通知，
  实测常被 Windows BTHLE 在 handler 接线完成前吞掉**（本机日志约半数连接收不到，
  如 02:51 那次全程无 `device_info`）。所以不能只依赖它。
- 因此补发 `battery_status_request`（与 30s 心跳同一机制）主动索要回包：固件
  `send_state_json` 受 `s_state_subscribed` 门控，收到回包即证明设备侧确实登记了订阅。
  fire-and-forget（僵尸链路上该写会挂到 OS 宣告链路死亡，不能阻塞连接协程）。
  健康链路实测 <1s 回 `battery_status`（连接总耗时因此 +0.7~0.9s）。

### 2. 僵尸拆除后必须按地址主动直连

`ScheduleZombieReconnect(session, delay)`：登记 `pending_proactive_reconnects_`（心跳
30s 周期兜底）+ 延迟线程提前唤醒（`WakeProactiveReconnectsAfter`，代数守卫同
`scan_epoch_` 手法），自愈压到秒级。

### 3. 自愈梯度（纯函数 + 有界）

`BleProtocol::PlanZombieHeal(light_attempts, full_repairs, full_repair_allowed)`
→ `kLightReconnect(A)` / `kFullRepair(B)` / `kUserAction`，host 单测 `TestPlanZombieHeal`：

| 级 | 动作 | 副作用 |
|---|---|---|
| A | 只 `Close` GattSession + BluetoothLEDevice 后按地址重连 | 无（不动系统配对） |
| B | `radio off/on`（`TryResetBluetoothRadioAsync`） | 一次全链路中断 ~5s；**系统配对保留** |
| 末级 | 保留现状，托盘气泡指引用户重配 | 打扰用户 |

故障期记账 `zombie_episodes_[addr]`（10min 窗，会话恢复即清零）：A 用满
`kZombieLightAttemptsBeforeRepair{2}` 次才升 B，B 用满 `kZombieMaxFullRepairsPerEpisode{1}` 次
才转用户；末级之后重连退到 `kZombieGiveUpRetry{60}` 一次，避免空转。

## 最重要的教训：自愈路径**绝不可** unpair

首版按仓库既有「stale bond 恢复」套路实现 B 级：`TryUnpairAsync` + radio reset +
`TryRestoreOsBondAsync`。真机结果：

```
02:59:13 zombie heal B: full repair VS-53A8 (unpair + radio reset + reopen + PairAsync rebuild)
02:59:21 zombie heal B: radio reset succeeded, reopening device
02:59:22 os bond restore VS-53A8: failed status=19 (HID passthrough stays dead …)
02:59:23 connected VS-53A8 / stage=ready          ← 语音自愈成功
```

随后核对注册表：`BTHPORT\Parameters\Devices\70041ddc53aa` 仍有密钥，但
`Enum\BTHLE\Dev_70041ddc53aa` 节点与 HID 设备**都消失了** ⇒ **按键直通被这次「自愈」弄死**。

事后单独复现 `PairAsync`（PowerShell + WinRT，`DeviceInformationPairing.PairAsync`）：

- 设备被 app 连上（NimBLE 连上即 `stop_advertising()`）时：**连续 12 次全部 status=19
  （`DevicePairingResultStatus_Failed`）**；
- 停掉 app、设备重新广播后仍失败（串口抓包显示**设备侧连连接都没发生** ⇒ Windows 侧
  根本没发起配对，属主机侧状态问题）；
- 对照：同日 01:54 用户经**配对对话框**（自带 `pair scan` 扫描看到广播后再
  `FromBluetoothAddressAsync` + `PairAsync`）**成功** `VS os pairing bonded`。

结论与整改：

1. **B 级改为只重置 Bluetooth radio**，系统配对一律不动 —— 设备侧 bond 在 NVS 里、
   Windows 侧 bond 也没变，重置后 HID 宿主用同一把 LTK 自行恢复按键直通。
2. 缺 bond 的重建改由**独立看门狗**（`RepairOsBondAsync`）负责：只读查询
   `Pairing().IsPaired()` 确认缺失后才动手，且要求「无会话 + 设备在广播」，
   `PairAsync` 重试 3 次；每次运行最多 1 轮（重建要拆掉健康会话，失败即白付一次语音空窗）。
   它**只补不删**，因此不可能把可用状态弄坏。
3. 若用户遇到「按键全死、语音正常」，先查上面那条注册表判据；修复路径仍是去 Windows
   蓝牙设置删除并重新添加设备（`PairAsync` 在此状态下无法自动完成）。

## 验证（真机 VS-53A8，2026-09-19 03:17-03:20）

连续 4 次 DTR 重启设备，**全程不改任何系统设置、不做人工干预**：

| 次数 | 重启 → `stage=ready` | 路径 |
|---|---|---|
| 1 | 03:17:19 → 03:17:29.8（**10.4s**） | 广告拆陈旧会话 → 安定窗 1.5s → 首连 `state subscribe timeout` → `[zombie-suspect: no cooldown, immediate retry #1]` → 二次连接收到 `device_info` → ready |
| 2 | 03:17:37 → 03:18:33.5 | 同上（重启被广告发现较晚；连接本身 5.5s） |
| 3 | 03:18:40 → 03:18:52.7（**12.7s**） | 同 1 |
| 4 | 03:18:59 → 03:19:57.3 | 同 2（连接本身 5.9s） |

- **A/B 级未被触发**（无 `heal level=full-repair` / 无 radio reset 日志）——轻量路径就够。
- 连接期活性证明在首版构建的真机循环里**已实测触发并正确判僵尸**
  （`zombie session VS-53A8: subscriptions reported success but device sent nothing within
  2500ms (device-side state_sub=0); heal level=light-reconnect(A)`），是本次自愈的入口。
- 构建 `build_win.bat` + CTest 2/2 通过。

## 二轮修复（2026-09-19 08:16-08:31，真机暴露的两个新缺陷）

系统配对恢复后重跑重启验证，暴露两处与「有系统配对」相伴的新问题：

### 缺陷 C：真断连路径只重扫，设备被 HID 宿主抢走后永久失联

`connection status VS-53A8 = disconnected` → `device disconnected …; restarting scan` →
**3 分钟零恢复**，随后日志出现 `XiaomiKeymapHook: raw input device not remote: …HID#{…}_70041ddc53aa…`
⇒ 设备已被系统 HID 宿主连上并 `stop_advertising()`，扫描永远等不到。
这与僵尸路径（D2）是同一个盲区，但发生在 `PlanZombieRecovery` 返回 `kScanOnly` 的分支上——
原修复只给僵尸分支补了主动直连。

**修复**：把按地址主动直连下沉到 `HandleDeviceDisconnected`（所有断连来源共用），
僵尸分支再用自愈梯度算出的延迟覆盖队列项。登记是无条件安全的：
`RunDueProactiveReconnects` 在设备未配对/已有会话/已有在途连接时自动清项，
失败按 `kProactiveReconnectRetry{60s}` 节流。

> **判据**：设备有 OS 配对时，「重启后 app 连不上」几乎必然走这条路径——HID 宿主会在
> ~1s 内抢走设备。看到 `restarting scan` 之后再无 `advertisement matched` 即为此症。

### 缺陷 D：故障期记账跨会话累积，重启后被一上来就判「自愈用尽」

日志：`heal level=user-action` 出现在设备刚重启、梯度本应从零开始的时刻。
原因：`zombie_episodes_` 只在**连接成功**时清零，而设备重启前后的多次僵尸判定落在
同一个 10min 窗内（重启前 3 次 + 重启后第 1 次即触发 `kUserAction`），此后重连被节流到
60s 且不再尝试 A/B —— 真正的故障期（设备重启后的全新链路）反而得不到自愈预算。

**修复**：`ClearZombieEpisode` —— 真实断连（`ConnectionStatusChanged=Disconnected`、
`GattSessionStatus::Closed`、心跳判 `kScanOnly`）时清空记账；`ProbeSessions` 里把
`HandleDeviceDisconnected` 放到 `NoteZombieEpisode` **之前**，避免僵尸路径自己把刚登记的
计数清掉（顺序敏感，注释已写明）。

修复后日志轨迹符合设计：`heal level=light-reconnect(A)` → 仍僵尸 →
`heal level=full-repair(B)` → `zombie heal B: radio reset` → 仍僵尸 → `kUserAction`（含气泡）。

### 测试装置教训：自动化 DTR 复位会让设备进入不可用状态

本轮用 pyserial 的 `dtr=False→True 0.3s→False` 序列自动重启设备，前几轮正常；
08:16 之后该设备 **USB 停止枚举**（`COM19` 消失、PnP 设备状态 Unknown），
而 BLE 侧仍被系统 HID 宿主持有 ⇒ app 的所有重连都「假成功」（GATT 无响应）。
这种状态下 radio reset / 换句柄都无效，**只能给设备断电重启**（拔插 USB 或按前面板键）。
教训：**连续自动复位设备做验证时要留人工兜底**，别在无人值守时反复 DTR 复位；
设备侧还有没有响应，看它是否还发 `advertisement`（`advertisement matched` 日志）比看 USB 更可靠。

## 长期技术记忆 / 经验

1. **「写成功」不等于「对端收到」**。BLE/ATT 应用的订阅、写入在加密上下文陈旧时会
   静默假成功；判定连接健康必须取对端的**反向证据**（收到的通知/回包），而非本机
   API 的返回码。同源先例：NimBLE 对未订阅连接 `ble_gatts_notify_custom` 也返回成功。
2. **自愈/恢复路径的力度必须按「破坏性」而不是按「彻底性」排序**。删系统配对看起来
   更彻底，实际是**不可逆地弄坏一条独立功能**（HOGP 按键直通），且重建所需 API
   （`PairAsync`）在故障态下恰好不可用。先做零副作用动作（回收句柄 / 重置无线电），
   把破坏性动作留给用户决策。
3. **不要用「重扫」兜底需要主动连接的场景**。设备作为 peripheral 一旦被任一宿主连上
   就停广播，纯等广告的恢复路径存在结构性盲区；按地址直连才是可靠兜底
   （同 `Doc/Expe/ble-startup-connect-failure-permanent-stuck-2026-09-14.md`）。
4. **首帧通知不可靠**。CCCD 刚订阅完成时的第一个通知常被 Windows BTHLE 在 handler
   接线窗口内吞掉；依赖「订阅后必有一帧」的判定要补主动探测（写一个小请求要回包）。
5. **诊断「Windows 侧到底有没有发起动作」要用设备侧日志**。串口运行时日志虽不稳，
   但 NimBLE 的 `GATT procedure initiated` 能读到；本次正是靠「设备侧连连接都没有」
   把 `PairAsync` 失败定位为主机侧状态问题，而不是设备拒绝配对。
6. **纯函数记账 + 有界梯度**让自愈可测可推理：把「力度选择」抽成
   `PlanZombieHeal` 后，边界（第几次升级、用满怎么办）用 host 单测锁住，
   真机上只需观察日志里的 `heal level=`。

## 遗留 / 观察项

1. **VS-53A8 的 HOGP 系统配对于 2026-09-19 03:00 被首版自愈删除，需用户手动重新添加一次**
   （Windows 蓝牙设置，或走 app 自己的「配对设备」对话框——那条路径 01:54 实测成功）；
   重配后 `IsPaired()` 为真，看门狗不再介入。
   **自动重建在本机该状态下无法完成**（`PairAsync` 恒 `status=19 Failed`），已排除的可能：
   - 设备被 app 连上（停广播）——已拆会话让设备重新广播，并在收到广播后 1.5s 内配对，仍失败；
   - Windows 侧僵尸链路 —— 已加 `TryResetBluetoothRadioAsync` 清链路状态后再配对，仍失败；
   - 主机侧根本没发起配对 —— 串口抓包显示设备侧零连接活动，故是 Windows 侧状态问题。

   **判据（新增）**：app 停掉 9s 后 `BluetoothLEDevice::FromBluetoothAddressAsync()` 仍返回
   `ConnectionStatus=Connected` = Windows 侧僵尸链路；此状态下 `PairAsync` 必失败。
   剩余怀疑方向：`Enum\BTHLE` 节点被删后 `DeviceInformation` 已无活体关联，需按 Settings 的
   DeviceWatcher / AssociationEndpoint 路径重建（未验证）。
2. **缺 bond 时目前只记日志，没有用户提示**（`kPairOsBondOptionalFailed` 文案已存在，
   但只在配对对话框里用过）。「静默失效」在本项上仍然存在，建议后续补托盘气泡。
   看门狗现状：`ProbeSessions` 每 10min 探测一次、每次运行最多重建 1 轮（`PairAsync` 重试 3 次），
   并在重建前重置无线电 —— 即使配对失败也**只补不删**，不会把可用状态弄坏。
3. 组件级 `heal level=full-repair(B)`（radio reset）**尚无成功样本**：08:31 那次是唯一
   真机样本，但当时设备已进入 USB 掉线/固件不可用状态（见「测试装置教训」），
   B 级执行成功却没能恢复 —— 该样本不能作为 B 级有效性的证据，需要在健康设备上重测。
4. 设备重启被广告发现的延迟波动较大（3s ~ 51s，串口 DTR 复位的时机不完全可控），
   自愈时长的主要构成不是 app 侧逻辑。
