# 小米网关语音键「过一段时间就用不了」：Windows CCCD 缓存让订阅根本到不了设备

> 日期：2026-09-19　相关 commit：`d05f1676`（缓存击穿修复 + 文档；该提交经 amend 落库）
> 相关文件：`desktop/windows/src/ble_central_win.cc`（`WriteCccdBestEffortAsync` 与两处订阅点）
> 设备：VS-53A8（网关模式，固件 2.3.9）+ 小米蓝牙遥控器 2 Pro
> 同主题：`Doc/Expe/ble-zombie-self-heal-2026-09-19.md`（僵尸自愈梯度）、
> `Doc/Expe/xiaomi-gateway-voice-key-and-hid-passthrough-2026-09-18.md`（语音键 vs HOGP 两条通道）

## 症状

网关模式用一段时间后，**遥控器语音键按下无反应/不出字**，而其他按键（HOGP 直通）正常；
app 侧表现为反复重连但每次都被判僵尸（`subscriptions reported success but device sent nothing`），
且**重启 app、重置无线电都救不回来**，只有设备断电重启或用户手动重新添加设备才能恢复。

本次真机实例：app 从 08:47 一直失败到 11:58（3 小时 11 分，约 180 次重连尝试全部失败）。

## 判据（一眼定位）

| 侧 | 判据 |
|---|---|
| **设备串口**（决定性） | `W (…) voice_ble: send_state_json gated: connected=1 state_sub=0 conn=1` —— 链路在、订阅没登记。**这条日志在设备正常收发时不会出现** |
| app | `state subscribe VS-XXXX status=Success` 但耗时只有 **十几毫秒**（正常 ATT 往返 ~100-500ms），随后零入站 |
| app | 连接阶段 `link-layer connected … after 0ms polls=0`（Windows 认为「已连接」） |

## 根因

**Windows 缓存了 CCCD（客户端特征配置描述符）的值。** app 调用
`WriteClientCharacteristicConfigurationDescriptorAsync(Notify)` 时，Windows 发现
「要写的值和我缓存里记的一样」，于是**本地直接返回 Success 而不发空口包**。

- 缓存里为什么记着 Notify？因为**上一次会话**里 app 成功订阅过（例如设备重启前）。
- 设备侧呢？NimBLE 在**连接建立时把 CCCD 状态清零**（per-connection 属性），所以设备
  这边是 `state_sub=0`。
- 两边状态不一致 ⇒ 订阅永远到不了设备 ⇒ `voice_ble_is_ready()` 恒假 ⇒ 语音键录音被拒
  （其他按键走 HOGP，另一条通道，不受影响 ⇒ 用户看到的就是「只有语音键坏了」）。

这也解释了此前一系列「僵尸会话」现象，**推翻了 roadmap 里「加密上下文陈旧」的假设**：

- 为什么**没做过系统配对**时反而正常？未配对设备会让 Windows 失效 GATT 缓存
  （`GattServicesChanged`，代码注释里早有记录），缓存没了就得真发空口包。
- 为什么**用户手动重新添加设备**能修？unpair 清掉了整份缓存。
- 为什么**设备断电重启**能修？重启后设备先广播、app 抢先连上（此时 Windows 尚未用
  HID 宿主链路重建缓存）。

## 修复

CCCD **缓存击穿**：订阅前先写一次 `None`，再写 `Notify`。

```cpp
// WriteCccdBestEffortAsync(characteristic, None, …)   ← 限时、best-effort、失败只记日志
auto op = characteristic.WriteClientCharacteristicConfigurationDescriptorAsync(Notify);
```

无论 Windows 缓存里记的是哪个值，两次写里至少有一次是「值变化」，必须下发到设备；
设备最终一定处于 Notify 状态。VS 路径（state/audio）与小米 ATVI 路径（control/audio）
四处订阅点都已应用。

## 验证（真机）

同一台设备、同一故障状态（已连续失败 3 小时）下换上新构建：

```
[BLE 11:58:36.121] link-layer connected VS-53A8 after 0ms
[BLE 11:58:36.420] state notify VS-53A8 len=239 …{"event":"device_info",…}   ← 订阅真的到了设备
[BLE 11:58:36.585] connected VS-53A8
[BLE 11:58:36.586] connect stage VS-53A8 stage=ready t=696ms
[BLE 11:58:36.614] state notify VS-53A8 … battery_status
```

设备串口侧同步由 `gated: state_sub=0` 变为正常 `GATT procedure initiated: notify; att_handle=30`
（不再有 gated 告警）。首次尝试即恢复，连接耗时 696ms。

## 长期技术记忆 / 经验

1. **「写 API 返回 Success」不代表空口发出了包**。Windows BLE 会对 CCCD 一类
   「状态型写入」做本地缓存短路；判断订阅是否真的生效**必须取设备侧证据**
   （设备日志 `state_sub` / 收到通知），或用限时未缓存读探针。
2. **CCCD 是 per-connection 属性，Windows 却按设备缓存**——两边对「是否已订阅」的
   认知天然会漂移。凡是「订阅一次、长期复用」的 BLE 应用都要防这一手；最便宜的
   通用解法就是**先写 None 再写目标值**，把写入变成必然的「变化」。
3. **排障时先看设备串口那一条 gated 日志**，比 app 侧「订阅成功」有用得多。
4. 网关模式下**语音键与 HOGP 按键是两条独立通道**（§1.10 已记录）——「其他键正常、
   只有语音键坏」应优先怀疑 app↔设备的 GATT 订阅，而不是遥控器或音频链路。

## 遗留 / 观察项

1. 本次修复改的是**桌面端**；若日后仍出现 `state_sub=0`，可考虑让固件在收到
   `control_rx` 任意写入时回应一个「未被订阅」的提示帧（走读/写而非 notify），
   让 app 无需依赖通知即可自查——当前靠 app 侧的活性证明已能发现，暂不做。
2. 小米 ATVV 直连路径（app 直连 RC-XXXX，非网关模式）同样套用了缓存击穿，但**未单独真机复现**该路径的失败样本。
