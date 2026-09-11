# RFC：小米遥控器 wechat 模式点按触发（方案 A：WeType 静默期规避）

- 日期：2026-09-12
- 状态：已确认方向（用户拍板走方案 A）
- 前置定案：`Doc/Expe/wetype-voice-injection-blocked-2026-09-11.md`（三轮终版）
- 分支：feat/voice-recognition-option

## 1. 背景与问题

WeType 启动语音会话需要「快捷键按住 + **普通键活动静默 ≥约 0.5~1.5s**」。小米遥控器语音键按住期间以 ~30ms 持续产生真实 F5 按键活动，LL 吞键只挡投递、挡不住活动时间戳刷新，静默条件永不满足——**hold 按住模式下这是物理性冲突，注入侧无解**（`NeutralizeHeldF5`（f20dcb72）已被证伪）。HID report 层拦截的 PoC 同时定案：report 流内核直通 hidclass，用户态改写不可达；内核过滤驱动需签名/高维护成本，否决。

**方案 A**：交互从「按住说话」改为「点按 toggle」——物理键流结束后注入已 100% 验证的按住配方，静默期自然满足。

## 2. 目标 / 非目标

**目标**：小米遥控器 + wechat 模式下，「点一下语音键 → ~1s 后 WeType 弹面板 → 对本机麦克风说话 → 再点一下 → 文字上屏」全链路可用。

**非目标**：
- 保住「按住说话」原生体验（物理性冲突，已定案不可行）
- 修改小米遥控器固件 / ATVV 协议（红线）
- WeType 侧任何配合改动
- 本机麦克风热键模式支持 wechat 输出目标（`HandleLocalMicHotkeyPressed` 的门控不动，本机麦只由小米 click 会话内部驱动）

## 3. 方案总览

```text
现状（hold_to_talk + 小米，死路）：
  按住语音键 ──F5 物理流(30ms×N)──▶ WeType 静默期被毒化 ──▶ 面板永不弹
  └─ ATVV 音频 ─▶ 虚拟麦（白流）

方案 A（click toggle + 本机麦）：
  第 1 击（按下+松开）
    物理流结束（suppressor 照吞）──▶ ATVV STOP 合成 button_click
    ──▶ 协调器启动 wechat 会话：auto_switch + renderer.Start + 立即 SendDown(ctrl+win down+40ms repeat)
    ──▶ 本机麦 Start，PCM 直写 wechat ring buffer
    ──▶ 0.5~1.5s 后 WeType 弹面板（静默期自松开起算，注入流不毒化——实验定案）
  用户说话（本机麦 → ring buffer → renderer → CABLE → WeType）
  第 2 击
    ──▶ 合成 button_click（复用 session_id）
    ──▶ StopWechatInputMethodSession：SendUp + 本机麦 Stop + renderer Stop + 切回设备
    ──▶ WeType 结束识别、文字上屏
```

关键时序合法性：`SendDown` 发出时刻物理 F5 流已结束；注入的 ctrl+win repeat 流不毒化静默期（2026-09-11 实验定案，干净状态 4/4 触发）；本机麦首帧（~100ms）远早于面板弹出（0.5~1.5s），无「弹框读静音」问题。

## 4. 详细设计

### 4.1 新配置字段：`wechat_input_method.session_model`

两个正交维度拆开：
- `trigger_mode`（已有）：**用户按键交互**——hold_to_talk / click_to_talk
- `session_model`（新增）：**第三方输入法会话模型**——`"hold"`（按住式，WeType：启动 SendDown+repeat 维持、停止 SendUp）/ `"click"`（点按式，Typeless：启动/停止 SendClick）

默认值推导（零破坏兼容）：未配置时 `session_model = (trigger_mode == click_to_talk ? "click" : "hold")`——现有 Typeless 用户（若有）与 hold 用户升级后行为不变。方案 A 用户显式配置：

```toml
[wechat_input_method]
trigger_mode = "click_to_talk"
session_model = "hold"
```

配置矩阵与行为：

| trigger_mode | session_model | 设备 | 行为 |
|---|---|---|---|
| hold_to_talk | hold（默认） | StickS3 | 现状（首帧后 SendDown / SendUp）✓ 不变 |
| hold_to_talk | hold（默认） | 小米 | 现状（死路，文档已知限制）不变 |
| click_to_talk | click（默认） | 任意 | 现状（click toggle + SendClick 对）不变 |
| click_to_talk | **hold** | 小米 | **方案 A（本 RFC）**：click toggle + 立即 SendDown / SendUp + 本机麦 |
| click_to_talk | hold | StickS3 | 顺带受益：click toggle + 立即 SendDown / SendUp + 设备麦（音频链路不变） |

### 4.2 XiaomiAtvvSession：`wechat_click_toggle` 按键语义

`Options` 新增 `bool wechat_click_toggle = false`，由 `win32_app.cc` 的 session options resolver 填写（会话创建时现取，配置热更对新连接生效）：

```cpp
options.wechat_click_toggle =
    config_.OutputProfileForDevice(device_id).target == OutputTarget::kWechatInputMethod &&
    config_.wechat_input_method.trigger_mode == InteractionMode::kClickToTalk &&
    config_.wechat_input_method.session_model == WechatSessionModel::kHold;  // 仅方案A组合
```

该模式下的状态流（复用现有状态枚举，不新增态）：

| 输入 | 现有行为（click_to_talk） | wechat_click_toggle 行为 |
|---|---|---|
| 按下（0x08/0x04） | 立即 button_down + kStreaming | 仅回 ACK，**不发事件**，进 kTapPending |
| 按住音频帧 | Opus 编码发 AudioFrame | **丢弃**（EmitPcmFrame 早退） |
| Tick 300ms 长按 | —（已在 Streaming） | **不确认长按**（长按也是一次 click） |
| STOP（kTapPending） | —（走 kStreaming 才有） | **合成 button_click**（带 session_id + duration_ms=按住时长）→ kReady，设 300ms 重开拒绝窗（防遥控器抖动） |
| 双击窗 | 窗内第二击合成 double_click | **不进 kWaitSecondTap**（第二击就是下一次 toggle） |

**session_id 复用协议**（toggle 状态内聚在 ATVV 侧，协调器停止匹配（`*event.session_id == *active_session_id_`）零改动即可命中）：
- `wechat_click_active_ == false` 的 STOP：`id = next_session_id_++`，记 `wechat_click_session_id_ = id`，置 active=true
- `wechat_click_active_ == true` 的 STOP：复用 `wechat_click_session_id_`，置 active=false
- `Stop()`（断开/MIC_CLOSE）：复位 `wechat_click_active_ = false`

异常自愈验证：启动失败（renderer 无虚拟麦）→ 会话未建立（active=false）→ 下一击 STOP 发「停止 click(id=N)」→ 协调器 active=false 不停 → 非 stale（无 last_stopped 记录或已过期）→ 走**启动**分支（id=N）→ 若成功则 ATVV 标记与实际一致；硬超时停会话后迟到的停止 click → 2s stale 窗内被 `IsStaleWechatStopClick` 忽略，窗外走启动（重开）。均自洽。

注：id 复用仅存在于 wechat_click_toggle 的 StateEvent 合成，不触碰 `Stop()` 注释的「session 计数递增不复用」不变量（该不变量约束 AudioFrame 帧匹配，本模式无帧）。

### 4.3 协调器改动

**a) click 启动路径（`HandleWechatInputMethodPrimaryButtonDown`）**：`StartWechatInputMethodSession` 成功后，若 `trigger_mode == kClickToTalk && session_model == kHold`：
- **立即 SendDown**（不等首帧；`wechat_hotkey_sent_down_ = true`，首帧分支的 `if (!wechat_hotkey_sent_down_)` 守卫天然跳过）。失败走既有 `hotkey_send_failed` 路径（停会话 + 报错 + ready）。
- **本机麦启动**（仅小米设备，判据见 c）：`local_mic_capture_->Start()`；失败 → ShowTimedMessage + Stop 会话（对齐 `HandleLocalMicHotkeyPressed` 的失败处理）+ `local_mic_active_session_id_` 登记。

**b) Stop（`StopWechatInputMethodSession`）**：
- 热键停止动作由 `session_model` 决定（替换现有 `trigger_mode == kClickToTalk` 判定）：hold → `SendUp()`；click → `SendClick()`。
- 本机麦收尾：`local_mic_capture_->Stop()`（幂等；**锁外**调用——`FeedLocalMicPcm` 抢 `audio_mutex_`，对齐 422 行死锁注释与 `HandleLocalMicHotkeyReleased` 的先停采集约定）+ `local_mic_active_session_id_.store(0)`。

**c) 「音频来自本机麦」判据**：小米设备（`device_info` 已上报 `hardware == kHardwareXiaomiRemote2Pro`，协调器 `paired_device_info` 已有登记）且会话走 4.3-a 组合。实现为成员谓词 `WechatSessionUsesLocalMic(device_id)`。

**d) PCM 路由（`FeedLocalMicPcm`）**：持 `audio_mutex_` 后先判 `wechat_input_method_active_`：是 → `wechat_ring_buffer_->Write(pcm)` 直通（跳过 slicer/encoder/HandleAudioFrame 的 ASR 管线——wechat 会话无 Opus 往返需求，且本机麦会话与 focused_app 本机麦会话互斥，无歧义）；否 → 维持现状。

**e) `wechat_hotkey_sent_down_` / `received_audio_frames_` / watchdog**：本机麦 PCM 不计入设备帧计数（wechat 本就不设 stall watchdog，仅 hard timeout 兜底，30s 级覆盖「忘点停止」）。

### 4.4 回滚 f20dcb72（独立提交，先行）

删除 `NeutralizeHeldF5` 全链：`wechat_input_method_hotkey.h/.cc` 的 `GetAsyncKeyStateFn`/`SetGetAsyncKeyStateForTest`/`NeutralizeHeldF5` 及 SendDown/SendClick 调用点；`core_tests.cc` 的 `TestWechatHotkeySendDownNeutralizesHeldF5`。已证伪（毒化源是活动时间戳非按住位），留着误导后人。

## 5. 测试计划（TDD，红-绿-重构）

**批 1（回滚）**：删 NeutralizeHeldF5 测试与实现 → 全量 CTest 绿。

**批 2（XiaomiAtvvSession）**，新增用例：
1. 按下（2 Pro 0x04 一体帧）无 StateEvent、无 AudioFrame；STOP 合成 button_click（id=N、duration>0）
2. 第二击 STOP 合成 button_click 且 id 复用 N；第三击 id=N+1
3. 按住 >300ms 后 STOP 仍是一次 click（Tick 不确认长按）
4. 按住期间音频帧全部丢弃（无 AudioFrame action）
5. STOP 后 300ms 内 MIC_OPEN 被拒（重开拒绝窗）；窗外接受
6. Stop() 后 wechat_click_active_ 复位（下一击发新 id）

**批 3（协调器）**，新增用例（fake hotkey / fake capture / fake renderer）：
1. click_to_talk + session_model=hold：click 启动后**立即** SendDown（不等音频帧），repeat 维持
2. 小米 click 启动 → 本机麦 Start 被调；第二击 → SendUp + 本机麦 Stop + renderer Stop
3. FeedLocalMicPcm 在 wechat 会话期写入 ring buffer（断言 renderer 读到 PCM）
4. capture Start 失败 → 会话回 ready + UI 提示、无 SendDown
5. StickS3 click（session_model=hold）设备音频帧照常进 ring buffer（回归）
6. trigger_mode=hold_to_talk 回归：首帧后 SendDown（时机不变）
7. session_model=click 回归：SendClick 对（现状不变）

**批 4（配置）**：session_model 解析/序列化/默认值推导（click_to_talk→click、hold_to_talk→hold、显式覆盖）；resolver 填 wechat_click_toggle 的真值表。

## 6. 提交切分

1. `refactor(微信输入法): 回滚无效的 F5 中和修复 NeutralizeHeldF5（f20dcb72 证伪）`
2. `feat(小米遥控器): wechat 模式点按触发——ATVV 按键折叠 click toggle + session_model 配置`
3. `feat(微信输入法): 小米 click 会话本机麦直通与立即注入`（协调器 + 配置 + 文档，视体量可与 2 合并）

配套：`Doc/Ref/desktop-config.md` 补 `session_model` 字段说明与方案 A 配置示例；经验文档已含三轮定案（无追加）。

## 7. 风险与开放问题

- **启动到面板弹出 0.5~1.5s 无反馈**：wechat 模式刻意不弹 VoiceStick 浮窗（避免与输入法面板打架）。接受；若真机体验差，后续可用气泡/托盘提示补一拍（不在本 RFC 范围）。
- **WeType 触发去抖波动**：实测 0.53~1.4s，偶发更慢。无解（WeType 内部行为），文档标注预期。
- **重开拒绝窗 300ms**：若用户快速连点 toggle（<300ms 间隔）第二击被吞，需再点一次。可接受（正常点按间隔 >300ms）。
- **本机麦质量依赖**：音频质量从遥控器 ADPCM 16kHz 换成系统默认麦克风（通常更好）。采集失败有显式提示。
- **`hold_to_talk` + 小米仍是死路**：不阻止不警告（配置矩阵表已说明）；若真机验收后用户在意，再评估设置项校验提示。

## 8. 修订（2026-09-11 真机定案）：click/hold 会话直连默认麦克风，删除 CABLE 绕行

真机验收发现 §4.3-c 的「本机麦直通 + auto_switch + renderer」音频管道有致命缺陷：停止顺序「SendUp → 600ms 宽限 → 停采 → 停渲染 → 切回默认设备」在 WeType finalize 进行到一半时拆除管道，TSF 宿主永久卡死——面板不退、文字不上屏、之后新会话连 `start_received` 都不再有（gen88/gen89 实证；旧顺序至少 5s abort 自愈，见 `Doc/Expe/wetype-finalize-wedge-cable-teardown-2026-09-11.md`）。

修订：**小米 + click_to_talk + hold 组合下，本端不再启动本机麦采集、不做 auto_switch、不渲染 CABLE**——WeType 弹框后直接采集默认录音设备（真实麦克风），与本机麦直通音频等价（采集端本就钉住同一个默认设备）且物理同构（gen77 物理按住测试干净收尾）。停止只 SendUp，无任何拆除动作。

- §4.3-c 的「本机麦直通」与 d 的「PCM 路由」作废；`StartLocalMicForWechatSession`、`FeedLocalMicPcm` wechat 分支、`SetPreferredEndpointId` 钉扎链删除（后者是 d4fbfd13 针对绕行回环的修复，绕行移除后失去意义）。
- 配置矩阵表不变（`session_model = "hold"` 语义不变）；`auto_switch_default_recording_device` 对本组合不再生效（仅服务 StickS3 BLE 经 CABLE 的路径）。
- StickS3 + click/hold（BLE 音频）路径保持 CABLE 管道不变，停止顺序约束（keyup 先于管道拆除）仍有回归测试覆盖。
- §7「本机麦质量依赖」改为「默认录音设备质量依赖」（同源同质量）；「采集失败有显式提示」作废（本端无采集）。

## 9. 修订 2（2026-09-12 凌晨，第二定案）：停止击改走鼠标 detach；按住流限时

直连麦克风修复后真机复验仍「停止击后面板不消失」。SendInput 注入实验矩阵 + WeType 诊断日志对照定案（见 `Doc/Expe/wetype-finalize-wedge-cable-teardown-2026-09-11.md` 追加节）：**WeType 语音热键只能启动/重启会话，keyup 与 ESC 均无收尾作用；面板唯一关闭路径是鼠标点击 detach（点击面板外任意处）**。且启动击的持续 repeat 按住会把任何关闭动作立即重新弹开面板（「永不消失」元凶）。

- 启动击：SendDown + repeat，**限时 2500ms 后自动松开**（覆盖弹框静默期 0.53~1.4s 实测分布）。必须限时：按住流持续期间，用户任何关闭面板的尝试（鼠标 detach/超时）都会被下一个 repeat keydown 立即重新弹开（「浮窗点关又弹出、输入法无法使用」真机复验）。松开后会话仍存活，文字提交由停止击的「新按住提交」保证，不再依赖松开时机。
- 停止击：SendUp → **「新按住提交」循环（SendDown+repeat 1.5s → SendUp）→ 1.5s → 注入鼠标左键（当前光标位置）→ 再 1.5s → 补一次左键点击**触发 detach 关闭。两次点击兜底：新按住重开的空会话首次 detach 常停在 `mode=detaching` 不完成、面板残留（真机复验：手动再点一次即稳定关闭）。机制：WeType 的 commit 只在「松开时语音仍活跃（或刚停 <~1s）」时触发，点击折叠停止击天然晚于语音结束（SendUp 不 commit，文字滞留 composition）——而一次**新按住会 commit 当前 composition（commit_after_end）并重开新会话**（真机日志实证），detach 随后关闭重开的空会话；与用户亲手点击同语义（WeType 会把点击回放给光标下的应用），且 detach 必须晚于 commit（提前 = 取消并清除未上屏文本）。
- §4.1 的「停止击 → SendUp 结束上屏」语义修正为「停止击 → SendUp → 新按住提交（1.5s）→ SendUp → 0.5s 后注入左键点击 detach 关面板」；文字上屏由「新按住提交」保证（与语音时序解耦）。
