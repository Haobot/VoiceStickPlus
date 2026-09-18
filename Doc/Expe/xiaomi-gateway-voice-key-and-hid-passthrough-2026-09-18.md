# 小米网关「语音键正常、其他按键全死」：两条独立通道 + 三处订阅/路由缺陷 + 一处系统配对缺失

> 日期：2026-09-18　相关 commit：`81d91195`（修复）、`d1ba6378`（交接）
> 相关文件：`firmware/components/audio_pipeline/audio_pipeline.c`、
> `firmware/components/gateway/src/gateway_hid_host.c`、
> `firmware/components/gateway/src/gateway_hogp.c`、
> `desktop/windows/src/ble_central_win.cc`（遗留项）
> 方案文档：`Doc/Plan/xiaomi-remote-stick-gateway.md` §6.3 问题 8-12
> 设备：VS-53A8（COM19）；遥控器：小米蓝牙遥控器 2 Pro

## 症状

StickS3 网关模式下接小米遥控器，出现两个并存症状，且**互相掩盖**：

1. 按遥控器**语音键** → 设备屏幕报 `Audio wait:ESP_ERR_NO_MEM`，录不进音、无文字上屏。
2. 遥控器**除语音键外所有按键**（音量/方向/OK/返回…）按下去电脑毫无反应。

最容易误导的地方：语音键后来修好了，于是现象变成「只有语音键有用」，看上去像 HID 侧完全不工作；
实际上**这两条路根本是两套独立通道**，必须分开验证。

## 日志判据（下次快速识别同类问题）

| 现象 | 判据 |
|---|---|
| 语音键录音失败 | `audio_pipeline: ext stream create 64000 bytes failed` → 屏幕 `Audio <step>: ESP_ERR_NO_MEM` |
| 按键到底有没有到设备 | `voice_stick: gw key usage=0x%04x down kind=N`（kind 0=键盘/1=Consumer/2=软件路由/3=截留） |
| HOGP 报告发没发出去 | `gw_hogp: hogp 无外设链路可下发`（本次新增） |
| 电脑侧有没有 HID 链路 | `Enum\BTHLE\Dev_<地址>` 节点是否存在；`BTHPORT\Parameters\Devices` 有密钥但 Enum 无节点 = 密钥残留、节点被删 |
| 设备侧几条外设链路 | `voice_ble: connected handle=N` 与 `hogp 下发 … 外设链路 N 条` |

## 根因（四处，独立）

### A. 语音链路：FreeRTOS 的 `xStreamBufferCreate` 只能拿内部 RAM

外部源 PCM 环形缓冲 `EXT_STREAM_BYTES = 2 * 16000 * 2 = 64000` 字节用 `xStreamBufferCreate()` 创建，**必然返回 NULL**：

- `xStreamBufferCreate` 内部走 `pvPortMalloc`，而 ESP-IDF 把
  `portFREERTOS_HEAP_CAPS` **硬编码为 `(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)`**
  （`components/freertos/heap_idf.c`）—— **PSRAM 根本不在候选里**，无论有多少空闲 PSRAM。
- 网关模式（NimBLE 控制器+host、LVGL、双连接、32KB 内部预留）下凑不出 64000 字节**连续内部** RAM。
- **原注释「>16KB 分配走 SPIRAM（`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`）」是错的**：该阈值只作用于普通
  `malloc()`（`heap_caps_malloc_default`），对 `pvPortMalloc` 无效。

**诊断陷阱**：该失败点在 `start_ext` 中位于 `s_last_error_step = "opus"` 之前，步骤标签仍停在
`"wait"`，屏幕显示 `Audio wait:ESP_ERR_NO_MEM`——而真正「等上一会话任务退出」超时返回的是
`ESP_ERR_TIMEOUT`。**定位时以错误码为准，不要被步骤标签带偏。**

修复：`xStreamBufferCreateWithCaps(EXT_STREAM_BYTES, 2, MALLOC_CAP_SPIRAM)`；并在网关模式入口
`audio_pipeline_external_prepare()` 预创建，把分配移出「按下→首帧」关键路径。

### B. HID 侧只认了 1/7 个带 notify 的 Report 特征

真机枚举小米 HID 服务：**25 个 Report(0x2A4D) 特征，其中 7 个带 notify**（props=0x1A）：
0x0064 / 0x006B / 0x0072 / 0x0076 / 0x007A / 0x007E / 0x0085。

- 发现阶段 `s_report_handle == 0` 只锁**第一个**（0x0064），只订它的 CCCD；
- 接收阶段 `NOTIFY_RX` 用 `attr_handle != s_report_handle` 过滤——落在其余 6 个
  Report 特征上的按键报文被**当成 ATVV 包转发/丢弃**。

修复：收集全部带 notify 的 Report 句柄 → 逐个订阅（顺序推进，一次只挂一个 ATT 事务，
避开小米 ~4.9s 的 L2CAP 参数请求窗口）→ 接收阶段用句柄集合判定。

### C. CCCD 句柄不能靠「特征值句柄 +1」猜

Report 0x0076 的真实 CCCD 在 **0x0078**，猜出来的 0x0077 被遥控器以 ATT 0x03
Write Not Permitted 拒绝（NimBLE 错误码 259）。更糟的是原实现「一个句柄失败就卡死整条链」，
后面 3 个特征永远订不到。

修复：改用描述符枚举（`ble_gattc_disc_all_dscs` 找 0x2902，与 `xiaomi_atvv_client` 同手法），
**枚举不到才回退直写 +1**；单个句柄重试到上限即**跳过继续**，不卡整条链。

### D. HOGP 直通只发第一条 role=slave 链路 + 电脑侧没有系统配对

- `gateway_hogp.c` 的 `first_periph_handle()` 取「第一条外设连接」。网关模式外设侧可能
  同时存在 Windows HID 主机与桌面端 app 两条 slave 链路，挑中 app 那条时
  **NimBLE 对未订阅连接返回成功但不下发**——报文静默消失。修复 = 向**所有** role=slave 连接广播。
- 更关键的一环在**系统侧**：BLE HID 直通要求目标机与设备有 **OS 级配对**。没有
  `Enum\BTHLE\Dev_70041ddc53aa` 节点就没有 HOGP HID 链路，设备发的报告无处可去；
  而语音键走 app 自己的 GATT 通道，不受影响 ⇒ 现象仍是「只有语音键有用」。

## 修复

| # | 位置 | 改动 |
|---|---|---|
| A | `audio_pipeline.c/.h` | `xStreamBufferCreateWithCaps(..., MALLOC_CAP_SPIRAM)` + 网关入口预创建 `audio_pipeline_external_prepare()` |
| B | `gateway_hid_host.c` | Report 句柄集合 + 逐个订阅 + 按集合接收 |
| C | `gateway_hid_host.c` | 描述符枚举找 0x2902，回退 +1，失败跳过 |
| D | `gateway_hogp.c` | 广播所有 role=slave 连接 + 无链路告警 |
| 顺带 | `gateway_hid_host.c` | `sec_cb` 不再每次连接无条件 `delete_peer`（只在加密失败时清键重配） |
| 顺带 | `gateway_hid_host.c` | 恢复 Report CCCD 订阅 + HID Control Point(Exit Suspend)，拆成独立开关默认打开 |

## 验证

真机（VS-53A8 / Windows，COM19 串口 + app 日志双向取证）：

```
I (1558) audio_pipeline: ext stream ready 64000 bytes (SPIRAM)
I (6718) gw_hid: 小米 HID 服务内带 notify 的 Report 特征共 7 个
I (7891) gw_hid: Report CCCD(0x0086) 直写 0x0001 rc=0（7/7）
I (7978) gw_hid: Report CCCD 订阅完成：成功 7/7（按键推送通道开通）
I (7980) gw_hid: 写 HID Control Point(0x0059)=Exit Suspend rc=0
```

用户在 Windows 完成系统级配对后：语音键端到端出字；音量/方向/OK/返回键经 HOGP 直通生效；
host 单测 120/120 + 112/112 通过。

## 长期技术记忆 / 经验

1. **语音键与 HID 直通是两条完全独立的通道**：语音键会话沿由 **ATVV Control 帧**驱动
   （`gateway_atvv_on_press`），其他按键走 **HOGP HID Report**。任何「语音正常、其他键全死」
   的现象，都应当**先怀疑 HID 订阅与系统配对**，不要往 ATVV/音频侧深挖。反之亦然。
2. **FreeRTOS 动态对象（StreamBuffer/Queue/Task）默认只能拿内部 RAM**。需要放 PSRAM 时
   必须用 `...WithCaps(..., MALLOC_CAP_SPIRAM)` 系列；不要指望 `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`。
   这与 §1.6 里 audio_task 栈是同一个坑的两种表现。
3. **发现阶段的「只取第一个匹配特征」是危险默认**。多实例 GATT 特征（HID Report 尤其）
   必须全量收集，否则会静默丢掉一部分数据流，且症状极具迷惑性。
4. **CCCD 句柄不要靠布局惯例猜**。同一设备上不同特征的描述符间距可以不一致；
   枚举 + 回退双保险，且失败必须跳过而非卡死整条链。
5. **NimBLE 对未订阅连接 `ble_gatts_notify_custom` 返回成功但不下发**——发送成功 ≠ 送达。
   需要显式的「无链路/未订阅」告警，否则报文静默消失。
6. **ble 直通类功能的验收必须包含「系统级配对存在性」检查**，不能只看设备侧日志。

## 遗留 / 观察项

1. ~~**桌面端失效恢复路径会删掉系统配对却不重建**~~ **已修（`e0db7c63`）**——保留原始记录：
   原状：
   `desktop/windows/src/ble_central_win.cc` 在 `IsLikelyStaleBondError` 命中或 GATT
   `Unreachable` 时执行 `TryUnpairAsync` 删除 Windows 配对并重置蓝牙 radio；
   而 app 对 VS 设备**从不重建 OS 级 bond**（`PairAsync` 只在小米遥控器路径
   `AttemptXiaomiOsPairing` 里调用）⇒ HOGP 直通静默失效。本次真机日志实锤：
   `13:31:42 attempting to remove stale Windows pairing for VS-53A8`（恰在 13:31 烧录之后）。
   修复：新增 `TryRestoreOsBondAsync`，在两条恢复路径（stale-bond / Unreachable）
   重开设备后补一次 `PairAsync`；小米遥控器不走该路径。
2. ~~**僵尸会话只重扫、不给用户出路**~~ **已修（`e0db7c63`）**：心跳超时曾只做
   `HandleDeviceDisconnected` + `StartScan`，但设备此时多已被系统 HID 宿主连上并停止
   广播，`voice_ble` 连上即 `stop_advertising()`，扫描永远等不到，应用卡死在无会话
   状态（表现为语音键毫无反应）。新增纯函数 `BleProtocol::PlanZombieRecovery(link_gone,
   silent_ms, timeout_ms, is_voice_stick)`（TDD 先行，五组断言覆盖断链/从未入站/边界/僵尸/小米），
   心跳据此区分 `kScanOnly` 与 `kRepairBond`；后者经 `on_session_zombie` 回调
   弹托盘气泡，明确指引用户到 Windows 蓝牙设置删除并重新添加设备（同设备一次
   故障期只提示一次）。真机验证已触发：`heartbeat teardown reason=no_rx_timeout` ⇒
   `"zombie session VS-53A8: bond repair required"` ⇒ `"stale session dev=VS-53A8: prompting"
   `"user to re-pair in Windows Bluetooth settings"`。
   **注意**：气泡只是把「静默失效」变成「可执行的指引」，僵尸本身仍需要用户手动到
   系统设置重配一次——自动化解法（僵尸即触发 unpair+radio reset+PairAsync 自愈）未做，
   因为失败时会把当前可用的按键直通一起弄坏，需另行评估。
2. **Windows 的 HID 主机与 app 复用同一条 ACL 链路**（app 重连日志
   `link-layer connected VS-53A8 after 0ms`），设备侧始终只有一条 peripheral 连接。
   因此二者共存**不需要**第二条连接，也**不需要**保持广播——
   `voice_ble` 连上即 `stop_advertising()` 不是问题，不要据此改广播策略。
   （此前交接文档里「MAX_CONNECTIONS 被 HID 主机+小米占满」的读法属误判。）
3. 设备完成系统级配对后，重启/重烧**不再**触发 app 的 stale-bond 删除路径（实测 14:29 烧录后
   无 unpair 日志）——该故障主要在「设备未 OS 配对 + 反复重启」的组合下出现。
