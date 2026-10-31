# 网关 P1 切换器 + 网关模式 OTA 攻坚（2026-09-20 全天复盘）

> 日期：2026-09-20　分支：feat/stick-gateway　设备：VS-53A8（网关模式，固件 2.3.9）
> 覆盖：Doc/Plan/xiaomi-gateway-p1-switcher.md（设计）、xiaomi-gateway-followup-roadmap.md（路线图）
> 关联经验文：ble-state-burst-mtu-truncation-2026-09-20.md、ble-zombie-self-heal-2026-09-19.md §五、
> ble-cccd-cache-subscription-not-delivered-2026-09-19.md 追加节

## 成果

1. **P1 网关切换器 2→6 步全部落地**：目标表（NVS 持久化 + gateway_target_info）、gateway_switcher
   纯逻辑 + host 单测、连接过滤 + 切换接 NimBLE、真机验收。切换 ≤2s（avg 690ms）、小米链路零断开、
   目标机恢复 2.5s（修复前 14–26s）。
2. **交互改版**（用户反馈驱动）：编码器长按菜单 → **侧键切换器**（短按预览浮窗 / 3s 内再按轮流切换 /
   录音中转发取消），编码器恢复纯「旋转+录音」。
3. **网关模式 BLE OTA 从「必死」到 100% 通过**：两个独立根因修复（见下），最终 1,548,592 字节全程写通、
   设备重启回连 778ms。
4. **验收 harness**：scripts/e2e_test/gateway_switch_acceptance.py（桌面端驱动 + 串口/日志双证据 + PASS/FAIL）。

## 关键根因（经验）

1. **订阅时 MTU 未协商就推大帧**（device_info/encoder_status/gateway_status 初始帧串）：att_mtu=23 ⇒
   预算仅 16B，三帧全截断 ⇒ 对端活性证明超时 ⇒ 判僵尸 ⇒ 免退避重试。修复=MTU≤23 时挂起、BLE_GAP_EVENT_MTU
   到了再推（+1.2s 兜底）。见 ble-state-burst-mtu-truncation-2026-09-20.md。
2. **OTA 两条路径的「裸 co_await 无超时」**：①ota_state 订阅裸 await ⇒ OTA 永远卡 0%（设备侧 subscribe
   attr=37 证明写已到、只是 Windows 回调没来）；②数据分块写裸 await ⇒ 中途停住无任何日志。修复=缓存击穿 +
   when_any 超时 + 进度停更检测。见 ble-cccd-cache 追加节。
3. **陈旧会话守卫的误杀**：Windows 广告 watcher 送延迟/复用报告（扫描开着时），守卫据此拆掉「正在 OTA 的
   活会话」⇒ OTA 每次 +30s/16% 断链。修复=「会话仍活着（1s 内有入站或 OTA 进行中）就不拆」。见 zombie §五。
4. **多连接下单值 current_peer 被误清**：网关模式设备同时维持 OS-HID + app 两条链路（MAX_CONNECTIONS=3），
   voice_ble 的 s_current_peer/s_connected/s_conn_handle 都是单值，任一链路断连即清掉 ⇒ 侧键预览显示
   No target。修复=显示三级兜底（已选目标 → 当前对端 → 表内最近目标）。**深修（按连接计数）未做**。
5. **交互设计**：编码器按钮本身就是录音触发，长按会拉起语音识别，故切换入口不能挂编码器长按——放侧键。

## 教训（操作/方法）

1. **本机整机冻结数小时**（12:19→15:37 日志时间戳跳变 3h18m）：冻结期间一切「停住/卡住/超时」观测都不可信；
   定位「卡住」类问题先用日志时间戳排除冻结，再判断是否真的停。
2. **多日日志跨天时间戳陷阱**：app 日志跨多日，用「时:分」筛行会命中历史天的同刻行，把历史故障当当前现象；
   必须同时确认日期/行号（曾因此误判「RC-6459 直连 ATVV 在刷日志」）。
3. **剪贴板测试与实机并发 flaky**：用户实机「语音粘贴」并发抢剪贴板时，TestClipboardVaultMultiFormatRoundTrip
   断言点随剪贴板内容漂移（两次失败不同格式）——非回归，需隔离剪贴板再根治。
4. **第三方 BLE central 驱动不了设备**：app 退出后 Windows 系统级 HOGP 配对把设备连走并停止广播，bleak 等
   既扫不到也连不上；自动化一律走 app（--control / --gateway-target）。

## 遗留

1. **非目标拒绝**：需第二台机器/第二适配器提供不同 identity address。
2. **多目标轮换**：只有单目标来回，未覆盖真实多目标维度。
3. **voice_ble 单连接状态机**在多连接下的清理语义（深修，本次用显示兜底规避）。
4. **剪贴板 flaky 测试**根治（隔离/独占剪贴板）。

（本文阈值/毫秒数/行号为 2026-09-20 记录时点结论，引用前以当前源码为准。）