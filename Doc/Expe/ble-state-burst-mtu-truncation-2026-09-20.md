# 订阅即推大帧撞 MTU 未协商：对端把健康会话误判成僵尸（网关切换后目标机 14–26s 才恢复）

> 日期：2026-09-20（修复日）
> 相关文件：`firmware/components/voice_ble/voice_ble.c`、`desktop/windows/src/ble_central_win.cc`
> 相关 commit：`cc82fb18`（修复）、`af9425a4`（soak 数据）、`6840449f`/`6af69632`（P1 目标表与桌面端目标选择入口）
> 相关方案：`Doc/Plan/xiaomi-gateway-p1-switcher.md` §4.2–§4.4
> 同族旧文：`Doc/Expe/ble-cccd-cache-subscription-not-delivered-2026-09-19.md`（Windows 缓存 CCCD 不投递）、
> `Doc/Expe/ble-zombie-self-heal-2026-09-19.md`（僵尸会话自愈梯度）

## 症状

P1 网关切换（设备主动断开当前目标 → 重新广播 → 目标重连）在设备侧只要 **0.45–0.9s**，
但**目标 PC 上的 app 要 14–26s 才回到 `stage=ready`**：中间 1–4 次连接尝试失败，
失败原因混杂 `state subscribe timeout after 2500ms`、`audio_tx discovery failed: AccessDenied`、
`notification subscriptions reported success but the device never registered them`（僵尸判定），
随后进入 zombie-suspect 免退避重试风暴。

起初（包括本文作者）把它当成「重连节奏 / OS 抢链路」问题——**方向错了**。

## 日志判据（下次同类问题直接照这个认）

设备串口：

```
I (28642) voice_ble: subscribe state desync: ... resyncing
W (28643) voice_ble: state json 235B + 4B header exceeds notify budget (att_mtu=23), peer will truncate
I (28673) NimBLE: GATT procedure initiated: exchange mtu      ← MTU 交换排在推送之后
```

三条一起出现即可定案：**订阅回调里推的帧 + `att_mtu=23` + MTU 交换在推送之后**。
桌面端对应的判据是 `state subscribe timeout after 2500ms`（订阅存活证明超时）——
注意它**既可能是设备侧没登记订阅（CCCD 缓存问题），也可能是帧被截断解析不出来**，两者的对端证据不同：
前者看 CCCD 写入耗时（17ms 假成功，见同族旧文），后者看设备侧 `exceeds notify budget`。

## 根因

`BLE_GAP_EVENT_SUBSCRIBE`（`state_tx` 订阅成功）到达时，固件立刻推送初始状态帧串
（`device_info` 235B + `encoder_status` 41B + `gateway_status` 43B）。
但**我们自己发起的 MTU 交换还在途**，`ble_att_mtu(conn)` 仍是默认 **23** ⇒ 单帧通知预算只有
`23 - 3(ATT) - 4(帧头) = 16B`。三个帧全部超预算被截断，桌面端拿不到任何可解析的状态帧，
其「订阅存活证明」（2.5s 内必须收到一帧可解析状态）超时 ⇒ 判为僵尸会话 ⇒ 走免退避重试。

要点：**这跟帧本身多大无关**——早先把 `device_info` 从 258B 精简到 235B（P4 记账的那次修复）
并没有解决问题，因为在 MTU=23 的窗口里连 41B 的 `encoder_status` 都过不去。
真正的判据是「推送时 MTU 是否已协商」，不是「帧有多小」。

## 修复（`voice_ble.c`）

- 订阅回调里：`ble_att_mtu(conn_handle) <= 23` ⇒ 置 `s_state_burst_pending`，不推；
  否则照旧立即推。
- `BLE_GAP_EVENT_MTU` 到达且有待发帧串 ⇒ 清标志并推（实测只晚 33–48ms，因为连接时
  固件自己就发了 `ble_gattc_exchange_mtu`）。
- 兜底：1.2s 一次性 `esp_timer`，防对端不响应 MTU 交换时静默不发（此时按旧行为发，不劣于修复前）；
  断连时清标志并停表。

## 验证（真机，2026-09-20 11:2x–11:35）

| 指标 | 修复前 | 修复后 |
|---|---|---|
| 目标机 app 断开→`stage=ready` | 14s / 26s | **0.92 / 2.53 / 2.54 / 2.53 / 2.54s**，全程零 `connect failed` |
| 设备侧 发起→`accept_peer (state=connected)` | 447–893ms | **9 轮 avg 690ms / worst 893ms** |
| 订阅时 MTU 截断告警 | 每次订阅必现 | **0 次**（改为 `state burst flushed after MTU exchange (att_mtu=247)`） |
| 切换期间小米 central 链路事件 | — | **0**（`gw_hid`/`gw_atvv` 行全部集中在开机 7.5s 内） |

验收 harness：`python scripts/e2e_test/gateway_switch_acceptance.py --rounds 5 --skip-manual`。

## 长期技术记忆 / 经验

1. **「对端报订阅成功但没收到帧」有两个完全不同的根因家族**：①对端没发出 CCCD 写（Windows 缓存，见同族旧文）；
   ②帧发出去了但被链路预算截断（本文）。诊断时**先看设备侧有没有 `exceeds notify budget`**，一眼分流。
2. **任何新增/扩容 `state_tx` 帧，必须按最小预算（MTU 23 ⇒ 16B JSON）算一遍**；超了就必须走「等 MTU 再推」
   的路子，而不是「把帧缩小一点」——缩小只推迟下一次踩坑。
3. **存活证明把「帧可解析性」和「会话健康」耦合在一起**：首帧被截断 ⇒ 对端判死链路 ⇒ 免退避重试风暴 ⇒
   用户侧表现为十几秒不可用。设计上推首帧的路径都要保证「要么完整到达，要么不发」。
4. 定位这类问题**别先怀疑重连节奏**：设备侧时延（0.5s）与用户侧时延（20s）差两个数量级时，
   瓶颈一定在对端会话建立，而不是链路本身。

## 附带结论：app 退出后第三方 BLE 探针驱动不了设备（E2E 相关）

想用 `bleak` 之类第三方 central 直接驱动设备做自动化时实测：**app 退出后 Windows 会用系统级 HOGP 配对
自动把设备连走**（设备随即停止广播），第三方 central 既扫不到也连不上。
所以：(a) 本轮为此补了**桌面端入口** `VoiceStick.exe --gateway-target self|clear`（复用 `--ota` 的
WM_COPYDATA 转发机制），自动化一律走 app；(b) **绝不能为测试去解除系统配对**——会弄死 HOGP 按键直通
（红线，见 `Doc/Expe/ble-zombie-self-heal-2026-09-19.md`）。

## 遗留 / 观察项

- **非目标拒绝路径未真机验证**：需要第二个 identity address（第二台机器/第二个适配器）。
  同一适配器做不到——设备按 identity address 识别目标，同机连接的 identity 相同。
- 失败窗口里出现的 `audio_tx discovery failed: AccessDenied` 疑为「OS HID 正在建立链路时 Windows 拒绝
  应用层 GATT 访问」，属**推断未证实**；本轮修复后该现象未再复现，暂不深追。
- 多**目标**轮换（两台 PC 之间切）仍未覆盖，本轮 9 轮是同目标来回（路径等价，目标维度未覆盖）。

（本文阈值/行号/毫秒数均为 2026-09-20 记录时点结论，引用前以当前源码为准。）