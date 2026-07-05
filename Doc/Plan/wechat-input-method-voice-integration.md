# Voice Stick 对接微信输入法语音输入（Windows）技术方案

## 1. 背景与目标

### 1.1 背景

当前 Voice Stick 桌面端通过 BLE 接收 Stick S3 采集并 Opus 编码的音频，再封装为 Ogg Opus 后走自有 ASR（火山/腾讯/ Voice Stick Cloud）进行识别，最后通过剪贴板 `Ctrl+V` + `Enter` 注入到当前焦点应用。该链路完整、可控，但识别准确率与体验受限于所选 ASR 服务。

用户发现微信输入法（Windows 独立版）的语音输入准确率明显优于当前 ASR，希望在不占用系统物理麦克风的前提下，把 Stick S3 作为微信输入法的语音输入源，同时保留现有 ASR 作为备份能力。

### 1.2 目标

- 新增一种全局输出模式 `wechat_input_method`，与现有 `focused_app`、`subtitle` 并列。
- 在此模式下：
  - Stick 主键按下 → 触发微信输入法语音输入快捷键；
  - Stick 通过 BLE 传输的音频经桌面端解码为 PCM 后，渲染到虚拟麦克风；
  - 微信输入法从虚拟麦克风取音，完成云端识别并自动上屏；
  - Stick 主键释放 → 释放微信输入法快捷键，结束送音。
- 不占用物理麦克风：Voice Stick 自身不再使用系统录音设备，仅通过 BLE 接收音频。
- 保留现有 ASR 链路，用户可在设置中随时切回。

---

## 2. 可行性分析

### 2.1 现有音频链路可直接复用

- BLE `audio_tx`（UUID `…5101`）已经以 16 kHz 单声道、40 ms/帧、Opus 编码传输音频（`firmware/components/audio_pipeline/audio_pipeline.c:26-34`）。
- 桌面端 `VoiceStickCoordinator::HandleAudioFrame` 已能按 session 接收并缓存 Opus payload（`desktop/windows/src/voice_stick_coordinator.cc:683-728`）。
- 当前桌面端只把 Opus 封装进 Ogg Opus，**尚未解码为 PCM**；本方案需要新增 Opus 解码能力。

### 2.2 微信输入法可被快捷键触发

- 微信输入法 Windows 独立版支持自定义语音输入快捷键（可在输入法设置中配置，例如 `Ctrl + Win` 或其他组合键）。
- Voice Stick Windows 端已通过 `requireAdministrator` 清单解决高 IL 进程（如微信 4.0）的 UIPI 注入问题（参考 `Doc/Plan/windows-uipi-elevated-injection.md`），使用 `SendInput` 发送全局快捷键在技术上是可行的。

### 2.3 音频注入必须依赖虚拟麦克风

- 微信输入法没有独立的麦克风设备选择界面，其语音输入默认使用 **Windows 系统默认录音设备**。
- 因此，要让微信输入法“听到”Stick 的音频，必须在系统音频栈中呈现一个虚拟录音设备，并将其设为默认，或在 Stick 录音期间临时把默认设备切换为该虚拟麦克风。
- Windows 下要创建一个被所有应用认可的真实虚拟麦克风，**需要内核态 WDM/AVStream 音频驱动**（WDK `MSVAD` 示例是最权威的参考实现）。纯用户态 WASAPI 注入只能针对特定音频会话，无法作为系统级麦克风被输入法识别，且稳定性极差。

### 2.4 核心限制："不占用物理麦克风"的边界

- Voice Stick 本身通过 BLE 取音，确实不会打开物理麦克风。
- 但由于微信输入法只能使用系统默认录音设备，若把虚拟麦克风设为默认，会议软件等“也使用默认设备”的应用会暂时无法取到物理麦音频。
- **结论**：我们能保证“Stick 不抢物理麦”，但无法让微信输入法同时只占用虚拟麦而不影响其他默认设备依赖者。产品层面需要明确告知用户这一限制，或建议用户为会议软件单独指定物理麦克风设备。

---

## 3. 总体架构与数据流

```text
Stick S3
   │  mic → I2S → ES8311 → Opus encoder (16kHz mono, 40ms)
   │  BLE notify audio_tx(...5101)
   ▼
Windows VoiceStickApp
   │  BleCentralWin → BleProtocol → AudioFrame
   ▼
VoiceStickCoordinator (wechat_input_method 模式)
   │  HandleAudioFrame → OpusDecoder → PCM ring buffer
   │  primary button_down → SendInput(微信输入法快捷键按下)
   ▼
WASAPI renderer
   │  写入虚拟麦克风 Playback 端（如 VB-CABLE Input）
   ▼
Windows 音频栈
   │  虚拟麦克风 Recording 端（如 VB-CABLE Output）被设为默认录音设备
   ▼
微信输入法语音输入
   │  快捷键按住期间从默认录音设备取音 → 云端 ASR
   ▼
目标输入框（由微信输入法完成上屏）
```

---

## 4. 桌面端改动点

### 4.1 新增依赖：Opus 解码

- 桌面端目前只有 Opus **封装**能力（OggOpusMuxer），没有 **解码**能力。
- 引入 `libopus`（C 库），在 `voicestick_core` 中新增 `OpusDecoder` 封装：
  - 输入：完整 Opus packet（来自 BLE 的 payload）。
  - 输出：16 kHz、16-bit、单声道 PCM 帧（每帧 640 样本）。
- 推荐在 `desktop/windows/src/` 新增 `audio_opus_decoder.h/.cc`。

### 4.2 新增：PCM 环形缓冲与 WASAPI 渲染器

- 新增 `PcmRingBuffer`：按 40 ms/帧对齐，缓冲 BLE 到达的 PCM，平滑抖动。
- 新增 `WasapiVirtualMicRenderer`：
  - 使用 WASAPI shared mode 以 16 kHz/16bit/mono 打开指定虚拟音频设备的 Playback 端；
  - 在 `primary button_down` 时启动渲染线程；
  - 从 `PcmRingBuffer` 读取 PCM 写入设备；
  - 在 `primary button_up` 时停止并清空缓冲。

### 4.3 状态机扩展

当前 `SessionState`：`kReady → kRecording → kFinalizing → kPendingConfirmation / ...`。

在 `wechat_input_method` 模式下，状态机简化为：

```text
kReady
  │ primary button_down
  ▼
kWechatRecording        // 已发送快捷键按下，正在向虚拟麦送音
  │ primary button_up
  ▼
kWechatFinalizing       // 发送快捷键释放，等待微信输入法上屏
  │ 约 1-2s 后
  ▼
kReady
```

- `kWechatRecording` 期间不再走 `OggOpusMuxer`、`ASRWebSocketClient`、LLM 精修。
- 取消/侧键行为复用现有逻辑：侧键单击取消当前录音并释放快捷键。

### 4.4 热键注入

- 新增配置项 `wechat_input_method.hotkey`，字符串形式，例如 `"ctrl+win"`。
- 桌面端启动时解析为 `INPUT` 数组；`button_down` 时发送按下序列，`button_up` 时发送释放序列。
- 需要处理好修饰键顺序（先按下修饰键，最后按目标键；释放时相反）。

### 4.5 默认录音设备切换（可选，POC 阶段建议手动）

- **方案 A（推荐 POC）**：由用户手动在 Windows 声音设置中将虚拟麦克风设为默认。Voice Stick 只负责渲染音频。
- **方案 B（后续增强）**：通过 `IPolicyConfig` / `MMDeviceEnumerator` 在 `button_down` 时把默认录音设备切换为虚拟麦克风，`button_up` 后恢复。风险是可能打断其他默认设备应用，需充分测试。

POC 阶段先采用方案 A，文档中写明操作步骤。

### 4.6 配置模型

新增 `[output]` 选项 `target = "wechat_input_method"`，并新增专属配置段：

```toml
[output]
target = "wechat_input_method"   # 可选 focused_app / subtitle / wechat_input_method

[wechat_input_method]
hotkey = "ctrl+win"               # 与微信输入法设置中保持一致
virtual_mic_playback_name = "CABLE Input (VB-Audio Virtual Cable)"
# auto_switch_default_recording_device = false   # 后续增强项
```

- 配置保持与现有 `device_*` 覆盖机制兼容：未来可扩展为按设备覆盖 `output.target`。

---

## 5. 固件 / BLE 协议 / macOS 影响评估

| 模块 | 影响 | 说明 |
|---|---|---|
| 固件 | **无需改动** | 音频采集、Opus 编码、BLE 传输链路完全复用。 |
| BLE 协议 | **无需改动** | `audio_tx`、`state_tx`、`control_rx` 帧格式不变。 |
| 固件 UI | 可选增强 | 可在 `wechat_input_method` 模式下让屏幕显示不同图标/提示，但这需要先新增 `ui_state` 状态值，涉及协议扩展，建议第二阶段再做。 |
| macOS | 本次不做 | 用户明确优先 Windows 独立微信输入法。macOS 如需后续跟进，需单独评估 BlackHole 虚拟音频驱动与微信输入法 Mac 版快捷键。 |
| 网站 / 发布流程 | 无影响 | 不改动版本号、OTA、网站文案。 |

---

## 6. 分阶段实施计划

### 阶段一：POC 验证（目标：证明端到端可行）

1. **环境准备**
   - 在开发机上安装 VB-CABLE（免费单设备版）或 Virtual Audio Cable。
   - 将 `CABLE Output` 设为 Windows 默认录音设备。
   - 配置微信输入法语音输入快捷键为 `Ctrl + Win`。

2. **新增 Opus 解码**
   - 引入 `libopus`，封装 `OpusDecoder`。
   - 单元测试：输入已知 Opus packet，输出 PCM 样本数与能量符合预期。

3. **新增 WASAPI 虚拟麦渲染**
   - 实现 `PcmRingBuffer` + `WasapiVirtualMicRenderer`。
   - 单元测试：写入正弦波 PCM，通过虚拟麦 loopback 读出后验证频率。

4. **新增 wechat_input_method 输出模式**
   - 在 `VoiceStickCoordinator` 中识别 `target == "wechat_input_method"`。
   - 主键按下：发送 `Ctrl+Win` 按下 + 启动渲染；主键释放：发送 `Ctrl+Win` 释放 + 停止渲染。
   - 取消/侧键行为适配。

5. **端到端验证**
   - 真机按住 Stick 主键说话，观察微信输入法是否正确识别并上屏。
   - 验证物理麦克风未被 Voice Stick 进程占用。

### 阶段二：产品化打磨

1. **配置 UI**
   - 在 Windows 设置对话框中新增输出模式选择、快捷键输入框、虚拟麦设备下拉框。

2. **默认设备自动切换（可选）**
   - 实现 `button_down` 切换默认录音设备到虚拟麦，`button_up` 恢复。
   - 增加降级：切换失败时弹窗提示用户手动设置。

3. **错误处理与遥测**
   - 虚拟麦未找到、WASAPI 初始化失败、快捷键发送失败等场景给出明确中文提示。
   - 日志记录 session 时长、送音字节数、渲染欠载/过载次数。

### 阶段三：自研虚拟音频驱动（可选，取决于 POC 结论）

- 若第三方虚拟音频线体验良好但分发/安装成本高，评估基于 WDK `MSVAD` 示例开发签名驱动。
- 需要 EV 证书、WHQL 签名、多版本 Windows 兼容性测试。
- 该阶段应作为独立子项目另立规划文档。

---

## 7. 边界条件与异常流

| 场景 | 预期行为 | 原因 |
|---|---|---|
| 虚拟麦克风未安装 | 启动时检测失败，弹窗提示用户安装 VB-CABLE 并设为默认 | 缺少前置条件，无法运行 |
| 用户未把虚拟麦设为默认 | 微信输入法听不到声音；录音结束无文字上屏。POC 阶段日志提示 | 微信输入法只能使用默认录音设备 |
| 微信输入法快捷键冲突 | 热键发送后微信输入法无响应。设置 UI 需让用户自定义并校验 | 快捷键可能被其他软件占用 |
| 主键按下后快速释放 | 发送快捷键按下后立即释放，微信输入法可能仍识别为一次短语音输入 | 行为由微信输入法决定，Voice Stick 只负责同步 |
| BLE 丢帧/延迟 | `PcmRingBuffer` 欠载时补静音帧，避免爆音 | 保持虚拟麦音频连续 |
| 虚拟麦设备名变化 | 通过设备 ID 或部分名称匹配，而不是完全字符串匹配 | 不同虚拟音频线名称不同 |
| 切换到其他输出模式 | 立即停止渲染、释放快捷键、清空缓冲 | 避免模式切换时音频串扰 |

---

## 8. 风险与回退

| 风险 | 说明 | 缓解 / 回退 |
|---|---|---|
| 微信输入法更新后行为变化 | 快捷键、麦克风使用策略可能变更 | 保持松耦合：仅通过热键+虚拟麦交互，不依赖微信内部 API；出现异常时回退到现有 ASR 模式 |
| 虚拟麦克风延迟/音质差 | 第三方虚拟音频线可能引入额外延迟 | POC 阶段测量端到端延迟；不达标则切换到自研驱动方案或改用低延迟 ASIO 虚拟设备 |
| 默认设备切换影响会议软件 | 若开启自动切换，Zoom/Teams 等使用默认设备的应用可能掉麦 | POC 阶段不自动切换，由用户手动设置并自担风险；后续如做自动切换需显著提示 |
| 内核驱动开发成本高 | 阶段三需要签名、兼容性测试 | 作为可选阶段，POC 成功后再决策是否投入 |
| Opus 解码引入新依赖 | 增加构建复杂度与体积 | 使用成熟 `libopus`，静态链接或 vcpkg/conan 管理 |
| WASAPI 线程优先级/卡顿 | 渲染线程若被抢占会导致音频断续 | 渲染线程设置 `MMCSS`（AvSetMmThreadCharacteristics）提升优先级 |

---

## 9. 验收标准

### 9.1 POC 阶段验收

- [ ] 开发机安装 VB-CABLE 后，Voice Stick 能向 `CABLE Input` 稳定渲染 16 kHz PCM。
- [ ] 按住 Stick 主键时，微信输入法语音输入界面出现并持续收音；释放后结束。
- [ ] 普通话短句识别准确率主观优于当前 ASR 配置。
- [ ] Voice Stick 进程在录音期间不打开系统物理麦克风（可通过 Process Monitor / 隐私设置验证）。
- [ ] 切换到 `focused_app` 模式后，原有 ASR 链路正常工作。

### 9.2 产品化阶段验收

- [ ] 设置 UI 可配置输出模式、快捷键、虚拟麦设备。
- [ ] 虚拟麦缺失或配置错误时有明确中文提示。
- [ ] 单元测试覆盖 Opus 解码、PCM 缓冲、热键解析。
- [ ] `build_win.bat` + `ctest --test-dir desktop/windows/build-x64 --output-on-failure` 全绿。

---

## 10. 前置检查清单

- [x] 数据模型：新增 `wechat_input_method` 输出目标及对应配置段。
- [x] 接口/API 契约：BLE 音频帧、Opus 解码输出、WASAPI 渲染输入、热键注入序列均已明确。
- [x] 边界条件与异常流：虚拟麦缺失、默认设备未设置、快捷键冲突、BLE 抖动、模式切换均已识别。

---

## 11. 参考来源

- 微信输入法电脑版语音输入快捷键与触发方式：[电脑语音输入文字快捷键](https://www.ekangw.net/a/diannaojiqiao/2022/1216/774701.html)、[微信输入法支持电脑语音输入](https://www.sina.cn/news/detail/5265805713354274.html)、[如何在电脑上高效使用语音输入](http://mp.weixin.qq.com/s?__biz=MzIyMDc0Njc2Mg==&mid=2247483860&idx=1&sn=b0ff9ffaf63f746b0a458ab7c15aeb52)
- 微信输入法 Mac 版快捷键与 Fn 键全局劫持讨论：[微信“劫持”Mac 全局 Fn 键作为语音输入快捷键](https://global.v2ex.co/t/1196085)
- Windows 虚拟麦克风与音频注入技术背景：[How to create a virtual audio input device to simulate a microphone on windows?](https://stackoverflow.com/questions/74907682/how-to-create-a-virtual-audio-input-device-to-simulate-a-microphone-on-windows)、[Virtual audio device driver - NTDEV](https://community.osr.com/t/virtual-audio-device-driver/26847)、[Microsoft WDK MSVAD sample](https://github.com/MicrosoftDocs/windows-driver-docs/blob/staging/windows-driver-docs-pr/stream/avstream-dma-services.md)
- macOS 虚拟音频驱动参考（后续扩展用）：[BlackHole - macOS audio loopback driver](https://github.com/ExistentialAudio/BlackHole)
