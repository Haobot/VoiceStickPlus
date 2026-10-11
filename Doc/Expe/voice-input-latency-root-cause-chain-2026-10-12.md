# 语音输入端到端延迟 25 秒——五根因链（松键丢事件 / 快档被拒 / 慢档迎录音 / 1.2s 倒计时 / 无兜底）

- 日期：2026-10-11 ~ 2026-10-12（排查、修复与真机验证完成）
- 相关文件：`firmware/main/main.c`、`firmware/components/voice_ble/voice_ble.c`、`firmware/components/audio_pipeline/audio_pipeline.c`、`desktop/macos/Sources/VoiceStickCore/VoiceStickCoordinator.swift`、`desktop/macos/Sources/VoiceStickApp/OverlayController.swift`、`desktop/macos/Sources/VoiceStickApp/AppFileLog.swift`（新增）、`desktop/windows/src/overlay_window.cc`、`Doc/Ref/volcengine-asr.md`
- 相关 commit：`fb09bec`（五连修一体提交）
- 关联：`Doc/Expe/mac-ble-ota-400ms-hid-scheduling-2026-10-02.md`（本文推翻其「无解」定案，见其追加节）、`Doc/Expe/ble-state-burst-mtu-truncation-2026-09-20.md`、`Doc/Expe/ble-ota-flow-control-cross-version-deadlock-2026-09-26.md`

## 症状

用户反馈「说完话到显示浮窗、再到粘贴到文本框所需的时间太长，以前是很流畅的」。真机实测三种劣化形态：

1. 3.4s 语音，松键后 **25.6s** 才粘贴（浮窗也迟到 ~25s）；
2. 物理按住 4.27s 却录了 **33s** 幽灵音频（松键丢失，直到下一次按键才收尾）；
3. 一次会话 **77s** 无响应，最后由 ASR 服务端报「客户端超过15秒未发送音频数据」。

## 日志判据（下次快速识别）

**macOS 桌面端**（2026-10-11 起有文件日志：`~/Library/Application Support/VoiceStick/VoiceStickApp.log`）：

- `Audio stream summary frames=N span=Xs recording=Ys`（`sendFinalOggChunkIfNeeded` 打点）：
  **span ≫ 说话时长 = BLE 链路慢档爬行**（本案 span 28-35s vs 说话 3.4s）；**frames=0 = 设备侧没发帧**。
- `Button up duration_ms`（固件计的物理按压时长）与桌面 `Button down→up` 间隔**对不上**（up 迟到数十秒）= 松键事件曾丢失/幽灵录音。
- `Enter pending confirmation` → `Pending confirmation hidden; committing paste` 的间隔 ≈ 确认倒计时常量（现 0.4s；旧 1.2s 时这一步是固定 +1.26s）。

**设备串口**（USB JTAG 115200）：

- `fast interval request failed rc=530`（= `BLE_HS_ERR_HCI_BASE 0x200` + HCI 0x12 Unsupported Remote Feature）= **请求范围含 Apple 不接受的 <15ms 间隔**（见 400ms 文档追加节）。
- `conn updated: interval=320 latency=4` 常驻 = 400ms 省电档，音频通知塌到 **~2.4fps**（正常 25fps）。
- `tx task exit: sent=N dropped=0` 但桌面 span 很长 = **设备发得快、链路送得慢**——问题在连接参数/链路，不在音频管线。
- `app event queue full: dropped type=…` = 事件被挤丢（修复后按键事件已是 critical，应显示 `stashed for retry` 而非 dropped）。

## 根因（五个独立根因叠加）

1. **固件按键事件被静默丢弃**：app 事件队列仅 12 深度，`APP_EVENT_FRONT/SIDE DOWN/UP` 不在 A7 关键事件白名单；编码器旋转风暴（每事件一条）挤满队列时松键事件直接丢 → 录音无人停、`button_up` 不发 → 幽灵录音（33s）/桌面永久等待。
2. **macOS 拒绝含 <15ms 的连接参数请求**（rc=530）：快档 `itvl_min=6`（7.5ms）的范围请求被 Apple 栈**按范围整体校验**拒绝，录音全程锁死 400ms+latency4 慢档，音频通知 2.4fps，3.4s 语音拖 28-35s 才送达——**用户感知的「识别很久」主体**。
3. **空闲即降 400ms 慢档 + macOS 应用参数更新 ~5s 延迟**：ready 态立即 request_slow（2026-05 引入的省电策略），录音开始的快档请求要 ~5s 才生效，3-4s 短句整段在慢档里爬行。
4. **最终文本卡 1.2s 确认倒计时才粘贴**：`showFinal` autoHideAfter=1.2s，隐藏回调才 `commitPendingPaste` → 浮窗→粘贴固定 +1.26s（跨端一致的旧行为，本次按产品要求压缩）。
5. **卡死会话无兜底**：音频静默时桌面端无限等 audio_end/button_up（audioEndTimeout 只在 button_up 后武装）；固件停录 sentinel 入队 `portMAX_DELAY`，BLE 拥塞时最坏 50 帧×300ms=15s+ 堵塞拖死收尾。

## 修复（commit `fb09bec`）

| # | 机制 | 常量/位置 |
|---|---|---|
| 1 | 按键四事件升 A7 关键事件（入队 20ms 等待 + 单槽 stash） | `app_event_is_critical`（main.c） |
| 2 | 快档请求下限 7.5ms→**15ms**、CE 提示 8/8→0/0 | `voice_ble_request_fast_interval` `itvl_min=12` |
| 3 | **活动即拉快档**（`note_activity` 幂等请求）+ **空闲 5 分钟才降档** | `IDLE_SLOW_DELAY_US=300s`；`request_fast_interval` 幂等化；CONN_UPDATE 授予慢参数自愈重申 5 次/连接；录音中 1.5s 周期重申 |
| 4 | 确认倒计时 1.2s→**0.4s**（macOS `showFinal` 与 Windows `ShowFinalCountdown` 同步，确认/暂停/取消语义不变） | `OverlayController.swift` / `overlay_window.cc` |
| 5 | 桌面录音静默看门狗 3s 收尾 + sentinel 入队 3s 有界（超时 reset 队列）+ tx 重试粒度 10ms→2ms（同 300ms 预算） | `recordingInactivityTimeout=3.0`；`TASK_EXIT_WAIT_MS` 复用；`TX_RETRY_DELAY_MS=2 / TX_MAX_RETRIES=150` |

**排障基建**（同 commit）：macOS 文件日志 `AppFileLog`（进程入口 dup2 stderr→`VoiceStickApp.log`，8MB 轮转一代，对齐 Windows `log.cc`）；音频流统计日志；`stop_recording` 串口打点（session + down_age_ms）。

## 验证（同一台设备同一天三轮对照）

| 指标 | 修复前 | 中间态（修 #2 后） | 修完 #2+#3 |
|---|---|---|---|
| 音频送达速率 | 2.4 fps（span 28-35s） | 12.6 fps（span 5.8s） | **26 fps 实时（span≈说话时长）** |
| 松键→浮窗 | ~25s | 2.8s | **18-62ms** |
| 浮窗→粘贴 | 1.26s | 0.43s | **0.42s** |
| 松键→粘贴 | **25.6s** | 3.3s | **0.44-0.48s** |

设备侧 `sent=81/49 dropped=0` 零丢帧；host 测试 7/7；VoiceStickTests 768/768。真机三轮实测，非推断。

## 长期技术记忆/经验

- **测延迟先量化分段再动手**：双端锚点（音频流统计 + stop_recording 打点）落地后，「慢在哪一段」五分钟定论；本轮前期靠猜浪费了大量往返。
- **macOS 统一日志对第三方 app 的 NSLog 脱敏**（`<private>`），排障必须走文件日志；且 **CLI 直接启动 app 会因 TCC 归因到 shell 导致蓝牙静默失效**（判据：进程日志零 CoreBluetooth 活动），测试一律 `open` 启动。
- Apple 校验连接参数请求是**按范围整体校验**：范围含 <15ms 即拒（rc=530），不是「选档」；固定值 7.5ms 同样被拒（2026-10-02 案）。范围下限 15ms 可过，Windows 侧取 min 也足够（15ms 间隔 = 66 事件/s vs 25fps 音频需求）。
- NimBLE `ble_gap_update_params` 返回值语义：BLE_HS_EALREADY=2（在途）、`BLE_HS_ERR_HCI_BASE(0x200)+HCI 码`，530=HCI 0x12；查表见 `nimble/host/include/host/ble_hs.h`。
- 排队论防线：TX 队列 50 帧（2s 音频）+ drop-oldest + sentinel 无界等待 = 慢链路下松键后十几秒才收尾；**收尾路径的每个等待都必须有界**，尾帧丢失优于会话悬挂（串口日志可量化丢失量）。
- 「以前很流畅」类**回归要先找环境变化**：本案代码 5 个月未动（慢档省电 2026-05 引入），劣化来自 macOS 升级后参数校验收紧；不要只盯代码 diff。

## 遗留/观察项

- 400ms 旧案的 **HOGP 系统配对**场景（bluetoothd 按 HID profile 调度 ACL）下 15ms 下限是否同样生效未复测；若仍被 `status=571` 拒绝，按平台限制走 USB 兜底。
- 2M PHY 请求保留（`ble_gap_set_prefered_le_phy`）收益未单独验证。
- 0.4s 确认倒计时是产品默认值：调整需**两端同改**（`OverlayController.showFinal` / `OverlayWindow::ShowFinalCountdown`）+ `Doc/Ref/volcengine-asr.md`。
- macOS 文件日志只在启动时轮转（fd 固定，运行中无法换文件）。
