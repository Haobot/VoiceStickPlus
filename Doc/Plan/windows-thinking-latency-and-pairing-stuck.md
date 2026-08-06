# Windows 桌面端稳定性：Thinking 延迟消除 + Pairing 误触发治理

日期：2026-08-06。涉及：`desktop/windows/src/voice_stick_coordinator.cc`、`desktop/windows/src/ble_central_win.cc`、`firmware/main/main.c`、`firmware/components/voice_ble/voice_ble.c`、`desktop/windows/tests/core_tests.cc`。

## 问题陈述

1. **Thinking 延迟**：松开录音键后，设备屏幕/悬浮窗的 Thinking 状态要 2~4s 才消除，但识别文字已先一步显示在悬浮窗。**用户确认未开启精修**（`refine_enabled=false`），故延迟不在精修路径。
2. **Pairing 误触发**：偶发按下说话无反应，设备端进入 Pairing，但此前已配对并连接（背景：同时连接 2 台设备）。

## 根因初判（待真机日志证实，不猜测）

### 问题1 时序（focused_app + 无精修）

```
button_up -> BeginWaitingForAudioEnd -> EnterFinalizing("waiting_audio_end")
          -> [设备 thinking、悬浮窗 Thinking...]  ← 计时起点
等 audio_end 帧到达 或 kAudioEndTimeout(2000ms) 超时
          -> SendFinalOggChunkIfNeeded -> EnterFinalizing("final_audio_sent")
ASR 服务端处理：on_partial 不断刷新悬浮窗文字（用户看到的"识别好的文字"）
          -> on_final -> FinishWithFinalText -> EnterPendingConfirmation
          -> CompletePendingPaste -> EnterReady  ← 计时终点（thinking 清除）
```

无精修时 `FinishWithFinalText` 经 `TransformText` 立即 `completion`，几乎无延迟。故 2~4s 必来自：
- (a) **audio_end 帧等待**：固件 drain 尾帧丢失/迟到，桌面端等 `kAudioEndTimeout=2000ms` 超时（参考记忆 `button-up-notify-overtakes-audio-drain`）。
- (b) **ASR final 返回延迟**：火山/腾讯 ASR 收完全部音频后 final 返回 1~2s（partial 期间文字已上屏）。
- (c) 其他（如 button_up 经 state_tx 抢跑、BLE 链路抖动丢 audio_end）。

### 问题2（固件单连接 + 桌面端多设备）

固件 `s_connected` 单 bool，断连即 `start_advertising()` + `ui_status_set_pairing()`。桌面端 `ble_central_win.cc` 已有四层防线（见 `Doc/Expe/ble-watcher-silent-death-pairing-stuck.md`）。仍偶发，候选：
- (a) BLE supervision timeout 断连（reason=8），设备回 Pairing 广播，重连窗口内用户按下。
- (b) WinRT 僵尸会话未及时拆除，设备广播无人接收。
- (c) watcher 静默失效复发或防线漏洞。
- (d) 2 台设备多设备场景下 watcher/claim 调度新问题。

## 目标（可自动化执行 + 用户配合边界）

让 AI 自动完成：加诊断日志 → 构建 → 分析日志 → TDD 修复 → 跑测试 → 打包提交。
用户只配合：真机复现问题1（一次）、真机触发问题2（偶发，正常使用至复现）、采集设备串口日志。

## 阶段1：诊断日志增强（AI 自动，无需用户）

### 1.1 问题1 端到端时序日志（`voice_stick_coordinator.cc`）

在关键节点加 `LogCoordinatorLine`（带 `SteadyNowMs` 相对时间戳），形成一条可解析的时序链：
- `HandlePrimaryButtonUp`：`button_up ts= duration=`
- `BeginWaitingForAudioEnd`：`wait_audio_end start ts=`
- `HandleAudioFrame`(IsEnd)：`audio_end recv ts=` / 超时分支：`audio_end timeout ts=`
- `SendFinalOggChunkIfNeeded`：`final_chunk sent ts=`
- `on_partial` 首次：`first_partial ts=`（后续 partial 不重复打，避免噪声）
- `on_final`：`asr_final ts=`
- `EnterReady`(paste_complete)：`ready ts=` + 汇总 `latency_breakdown` 各段耗时

目标：一次复现即可判定 2~4s 耗在 (a)/(b)/(c) 哪段。日志加在 `voicestick_core`，不触碰 UI 层。

### 1.2 问题2 BLE 多设备诊断日志（`ble_central_win.cc` + 固件）

现有 `LogBleLine` / `disconnected reason=%d` 较全，补充：
- 断连/重连时打印当前所有已配对设备的连接状态快照（`VS-XXXX connected=... session_ready=...`），看清多设备视角。
- watcher 健康检查（`CheckScanHealth`）触发时打印多设备聚合状态。
- 固件 `disconnect` 补连接时长（`connected_ms - s_adv_started_ms` 已有，补 `since_connect`），辅助判 supervision timeout vs 主动断。

## 阶段2：真机采集 + 根因定位（用户配合）

### 2.1 问题1（易复现）
- AI 构建+运行带日志版本（`build_win.bat` → 重启 VoiceStick.exe）。
- 用户复现一次"松开后 2~4s Thinking"。
- 采集 `%LOCALAPPDATA%\VoiceStick\VoiceStickApp.log`，AI 解析时序链定位耗时段。

### 2.2 问题2（偶发）
- 用户正常使用 2 台设备。
- 遇"按下无反应+设备 Pairing"时同时采集：
  - 设备串口日志（USB JTAG，`disconnected reason=` 与重连时序）。
  - 桌面端日志（`VoiceStickApp.log` 的 BLE 段）。
- AI 双端时间对齐，判定 (a)/(b)/(c)/(d)。

## 阶段3：修复（TDD，根因确认后）

依阶段2 证据修复，**每个修复先在 `core_tests.cc` 写失败测试（红）→ 最小实现（绿）→ 重构**：

- 问题1：
  - 若 (a) audio_end 超时主导 → 排查固件 drain 尾帧路径（`audio_pipeline_stop` 等 drain 完成），或评估 `kAudioEndTimeout` 调整。
  - 若 (b) ASR final 延迟主导 → 等 final 期间 UI 反馈优化（设备/悬浮窗反馈更准确，避免"文字已上屏却显示 Thinking"的割裂）。
  - 若 (c) → 针对 button_up 抢跑/丢帧修复。
- 问题2：
  - 若 (a) supervision timeout → 链路参数/重连策略调整。
  - 若 (b) 僵尸会话 → 强化拆除/重连兜底，对照四层防线找漏洞。
  - 若 (d) 多设备 → watcher/claim 多设备适配。

## 阶段4：验证

- `ctest --test-dir desktop/windows/build-x64 --output-on-failure` 全绿（含新增测试）。
- 问题1：真机复现，确认 Thinking 延迟消除或降至可接受。
- 问题2：真机长时间使用，确认偶发 Pairing 不再出现。
- 按项目惯例打包签名 MSI 并 `git add -f` 提交（`desktop/windows` 被 gitignore）。

## 自动化边界（明确告知用户）

| 步骤 | 执行者 |
|---|---|
| 加诊断日志、构建、跑单测、分析日志、修复、测试、打包提交 | AI 自动 |
| 真机复现问题1（1 次） | 用户 |
| 真机触发问题2（偶发，正常使用至复现）+ 采集设备串口日志 | 用户 |

## 风险与回退

- 诊断日志为 `LogCoordinatorLine`/`LogBleLine`，可常驻（量小），无需回退。
- 修复改动经单测保护；固件改动需 `idf.py build` + 真机验证。
- 若阶段2 证据与初判不符，回到阶段1 补充日志再定位，不强行修复。

## 阶段1 实施记录（2026-08-06）

### 诊断日志（已加）
- **问题1 时序探针**（`voice_stick_coordinator.cc/h`）：`button_up`/`audio_end`/`final_chunk`/`first_partial`/`asr_final`/`ready` 各点 `SteadyNowMs` 时间戳存 atomic 成员，`CompletePendingPaste` 经 `LogLatencyProbeBreakdown` 汇总打印 `wait_ae_ms`/`chunk_to_first_partial_ms`/`asr_proc_ms`/`total_ms`，定位 2~4s 耗在哪段。集成环境实测 `total_ms=445`（`wait_ae=2`、`asr_proc=440`），证明探针工作正常。
- **问题2 多设备快照**（`ble_central_win.cc/h`）：`LogConnectionSnapshot` 在连接就绪/断连/扫描重建三处打印所有配对设备会话状态（`ready`/`session,!ready`/`no_session`）。
- **固件连接时长**（`voice_ble.c`）：`disconnect` 打印 `conn_dur`，辅助判 supervision timeout vs 主动断。

### Log 无锁并发 SegFault 修复（预存缺陷，被探针触发）
集成测试带探针版第二个用例 SegFault（原 coordinator 12 用例全通过，基线确认）。根因：`log.cc` 的 `Log` 函数**无锁**，探针在 ASR 回调线程新加 `LogCoordinatorLine` 调用，与 hotword extraction 后台线程、主线程并发写同一日志文件，`ofstream` 无锁并发写是未定义行为致 SegFault。修复：`log.cc` 加 `std::mutex g_log_mutex`，`Log` 加 `lock_guard`。ctest 确认通过（2026-08-06）：`voicestick_windows_tests` 19.87s 通过，`voicestick_integration_tests` 108.88s（12 语料）通过，SegFault 已消除。该修复属预存缺陷修复，与本次两个问题的根因无关，但为探针安全采日志扫清障碍。

## 阶段2 问题2 根因定位（2026-08-06，基于桌面端日志）

探针版运行日志（`%LOCALAPPDATA%\VoiceStick\VoiceStickApp.log`）揭示问题2 真实表现：

### 现场序列（VS-580C，07:08:42-07:08:47）
1. `07:08:42.808` 最后一次 state event（battery_status，链路正常）
2. `07:08:47.586` 收到 VS-580C 广告（`advertisement ... while session still registered`）-- **4.8s 后**
3. `07:08:47.592` 才报 `device disconnected`（WinRT 未投递 disconnect 事件，靠广告检测发现）
4. `07:08:47.596` `reconnect settle 1500ms`（等 OS 拆僵尸链路）
5. 重连成功

4.8s ≈ slow interval `supervision_timeout=500`(5s)：链路抖动 >5s 触发 supervision timeout 断连。

### 关键差异：VS-580C vs VS-D63C
- VS-580C：~60s 周期断连，20 次/小时
- VS-D63C：1 次/小时（稳定）
- 两台固件相同（supervision_timeout=5s）

**结论**：5s supervision_timeout 偏短是必要条件，但 VS-580C 频繁断连的诱因是其链路质量（信号/干扰/距离等物理因素）-- 链路每~60s 抖动 >5s 触发断连；VS-D63C 链路稳定不触发。延长 supervision_timeout 是**预防性缓解**（扩大抖动容忍窗），非根因修复；VS-580C 物理因素需另行排查。

### WinRT 僵尸会话（印证固件注释 voice_ble.c:551-554）
断连后 WinRT 不投递 `ConnectionStatusChanged` 事件，桌面端靠广告检测 + 1500ms settle 重连（~5-6s 窗口）。heartbeat 90s 来不及兜底（断连~60s < 90s），但广告检测~5s 已是主兜底。窗口内用户按下无反应 + 设备 Pairing UI。

## 阶段3 问题2 预防性优化（2026-08-06）

用户决定跳过固件串口采集（VS-580C 未连 USB JTAG，VS-D63C 几乎不断连采集效率低），基于桌面端证据做预防性优化。

### 改动：延长 slow interval supervision_timeout
`firmware/components/voice_ble/voice_ble.c` `voice_ble_request_slow_interval`：
- `supervision_timeout` 500(5s) -> 1000(10s)
- BLE 规范安全性：`(1+latency)*2*itvl_max = (1+4)*2*0.4s = 4s`，5s 余量仅 1s，10s 余量 6s
- fast interval（录音时 2s）不动：录音时链路活跃，2s 足够检测真断连

**预期效果**：VS-580C 链路抖动 5-10s 不再触发断连，减少误断连频率；真断连（设备关机/超出范围）检测延迟 5s->10s，靠桌面端广告检测+心跳兜底。

### 放弃的方案（基于证据否决）
- **1547 unpair 守卫**（原计划）：现场全程无 unpair/Unreachable，基于错误假设，放弃。
- **桌面端 heartbeat 缩短**：广告检测~5s 已是主兜底，heartbeat 90s 是静默死亡兜底（无广告场景），缩短增加流量收益有限。
- **桌面端 settle 1500ms 缩短**：会致撞僵尸链路重试（更慢），不动。

### 验证
- `idf.py build` 编译通过（100.20s，9 文件）
- 真机烧录 VS-580C + VS-D63C 长时间使用确认 VS-580C 断连频率下降：待用户配合（sticks3-flash-ota）


