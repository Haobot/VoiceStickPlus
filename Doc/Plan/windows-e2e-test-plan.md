# Voice Stick Windows 端端到端测试方案

> 版本：草案 v1　|　日期：2026-07-15　|　状态：待批准
>
> 目标：建立一套最大化自动化的端到端测试闭环，覆盖 ① ASR 识别->文本注入 链路，② 微信输入法模式（Opus 解码->虚拟麦克风渲染）链路，并打通固件真链路联调。

## 1. 背景与目标

Voice Stick 是「StickS3 固件 + Windows 桌面端 + ASR/LLM 云服务」的软硬件协同系统。当前测试仅停留在 `core_tests.cc` 纯逻辑单测层，缺乏：

- 跨软硬件链路的端到端验证；
- ASR 识别准确率的真实凭据验证；
- 微信输入法模式音频渲染的可观测验证；
- 可重复、可脚本化驱动的测试输入（目前只能人工按键+真人说话）。

本方案构建分层测试体系，从纯软件注入到固件真链路，逐层逼近真实，最大化自动化覆盖，对无法自动化的部分（微信黑盒识别）给出明确的人工抽检清单。

**已确认的前置条件**（用户拍板）：

- ASR provider：火山引擎（volcengine），用户提供 API key。
- 微信输入法 + VB-CABLE 环境：全部就绪。
- 微信模式验证深度：音频层 loopback 自动 + 关键用例人工抽检。
- 固件改造：同意，做完整 L3 真链路联调。
- 硬件通道：StickS3 经 COM17 串口连接，桌面端可经蓝牙 BLE OTA。

## 2. 总体架构：分层测试金字塔

```
            ┌─────────────────────────┐
   L4       │  微信输入法真机验证       │  人工清单 + loopback 抽检
            └─────────────────────────┘
          ┌───────────────────────────────┐
   L3     │  固件真链路联调（软硬件）       │  回放PCM->ES8311编码->BLE无线->桌面
          └───────────────────────────────┘
        ┌─────────────────────────────────────┐
   L2   │  微信模式渲染测试（桌面端集成）      │  Opus解码->TimedFakeSink/loopback
        └─────────────────────────────────────┘
      ┌───────────────────────────────────────────┐
   L1 │  ASR 链路集成测试（桌面端集成）             │  FakeBle注入->火山ASR->抓取程序
      └───────────────────────────────────────────┘
    ┌─────────────────────────────────────────────────┐
   L0│  现有纯逻辑单测（core_tests.cc）                 │  状态机/协议/mux/配置
    └─────────────────────────────────────────────────┘
```

| 层 | 注入点 | 覆盖链路 | 自动化 | 依赖硬件 |
|---|---|---|---|---|
| L0 | 函数调用 | 纯逻辑 | 全自动 | 否 |
| L1 | FakeBleCentral 注入 Opus 帧 | 桌面端 Ogg 封装->ASR->注入 | 全自动 | 否 |
| L2 | FakeBleCentral 注入 Opus 帧 | 桌面端 Opus 解码->渲染 PCM | 全自动 | 否 |
| L3 | 固件回放预存 PCM | ES8311->Opus 编码->BLE 无线->桌面全链路 | 半自动 | 是 |
| L4 | 真实麦克风/回放 + 真微信 | 含微信输入法识别 | 人工抽检 | 是 |

## 3. 输入侧：测试录音的脚本化驱动

### 3.1 软按键（已就绪，零改造）

固件 BLE `control_rx` 已支持 `{"event":"remote_button_down","button":"primary","request_id":N}` / `remote_button_up`（`firmware/main/main.c:722-729`），走 `APP_INPUT_SOURCE_REMOTE` 源，**跳过双击检测、跳过 300ms hold 阈值、跳过 hold_to_talk_instant 分支**，直接同步驱动 `start_recording`/`stop_recording` 并上报 `button_down`/`button_up`（`main.c:848-1075`）。

测试脚本通过桌面端 BLE 或独立 BLE 工具下发该命令即可脚本化驱动录音启停，无需物理按键。

> 限制：仅 `primary` 可远程注入，`secondary`（侧键）无远程入口。侧键相关用例（取消、恢复确认）需物理按键或固件新增 secondary 远程命令（L3 可选附带）。

### 3.2 测试音频来源（三层注入策略）

| 路径 | 注入点 | 用途 | 改造 |
|---|---|---|---|
| **B 桌面端注入** | FakeBleCentral 模拟收到 audio_tx Opus 帧 | L1/L2 桌面端集成测试 | 零固件改造 |
| **A 固件回放** | 固件 audio_task 用预存 PCM 替代 `esp_codec_dev_read`（`audio_pipeline.c:333`） | L3 真链路 | 改固件+烧录 |
| **C BLE 外设模拟器** | 测试程序用 Windows BLE GATT 扮演 StickS3 | 可选，补 L3 与 L1 之间的真实 BLE 协议层 | 中（本方案不首选） |

本方案 L1/L2 用路径 B，L3 用路径 A。路径 C 作为后续可选增强。

## 4. 输出侧：观测与断言基础设施

### 4.1 观测源清单（已确认）

| 观测源 | 位置 | 用途 | 开关 |
|---|---|---|---|
| Windows app 日志 | `%LOCALAPPDATA%\VoiceStick\VoiceStickApp.log` | 状态机流转、`LogWechatLatency` 各阶段时间戳、ASR 错误 | 默认开 |
| 调试音频落盘 | `%LOCALAPPDATA%\VoiceStick\DebugAudio\*.ogg`（`{时间}-VS-{id}-session-{sid}.ogg`） | **ASR 实际收到的 Ogg Opus 流**，可离线重放/比对 | `debug_audio_cache`（默认 false，测试时开启） |
| 固件串口日志 | COM17 USB JTAG 115200 | 固件侧事件（不稳定，关键数据走 NVS+BLE 上报） | 默认开 |
| WASAPI loopback | CABLE Input 渲染端 | 微信模式渲染的 PCM 抓取 | 新增工具 |
| 抓取程序 | 前台窗口接收粘贴文本 | ASR->focused_app 注入验证 | 新增工具 |

### 4.2 关键设计决策

- **字幕窗不可抓取**：字幕窗是 Direct2D+DirectWrite 自绘（`subtitle_window.cc`，`ID2D1RenderTarget`+`DrawTextLayout`），`GetWindowText`/UI Automation 均不可靠。ASR 内容断言不走字幕窗，改走抓取程序 + 日志 + 调试 ogg。
- **抓取程序用 Return 作分隔符**：`InputInjectorWin::Paste` 默认 `press_enter=true`（`input_injector_win.cc:31-34`），注入文本后发 Return。抓取程序拦截 Return 作为「一条识别结果提交」信号，天然分隔多轮。

## 5. 各层详细方案

### 5.1 L0 现有纯逻辑单测（已就绪）

`desktop/windows/tests/core_tests.cc`，基于 assert，Fake 设施齐全（FakeBleCentral/TimedFakeSink 等）。运行：`ctest --test-dir desktop\windows\build-x64 --output-on-failure`。本层不改动，作为回归基线。

### 5.2 L1 ASR 链路集成测试（新建）

**目标**：验证「桌面端收到 Opus 帧 -> Ogg 封装 -> 火山 ASR -> 识别文本 -> 注入前台」全链路，用真实火山凭据，断言识别准确率。

**实现方式**：新增独立测试可执行文件 `voicestick_integration_tests`（链接 `voicestick_core` + 真实 `asr_client`），区别于纯 Fake 的 `core_tests`。

```
[测试语料 ogg] -> OggOpusDemuxer 解封为 Opus 帧序列
              -> FakeBleCentral 模拟 audio_tx notify 投递给 coordinator
              -> coordinator 封装 Ogg -> 真实火山 AsrClient
              -> 识别结果 -> InputInjector 注入前台抓取程序
              -> 抓取程序收到文本 -> 比对预期文本（CER）
```

**关键复用**：`OggOpusDemuxer`（core_tests.cc:2659+）、`FakeBleCentral`（core_tests.cc:70）、`EncodeOpusPacket`（core_tests.cc:2416）。

**断言标准**：字错误率 CER（中文按字，用编辑距离 / jiwer 或 difflib）。通过阈值：常规语料 CER < 10%；含数字/专有名词语料用关键实体包含判定。

**配置驱动**：火山 key 从 `%APPDATA%\VoiceStick\config.toml` 读取（`asr_provider="volcengine"` + `volcengine_api_key`）。测试启动前校验凭据非空。

### 5.3 L2 微信模式渲染测试（新建）

**目标**：验证「桌面端收到 Opus 帧 -> 解码为 PCM -> 渲染到虚拟麦克风」的解码正确性、首帧延迟、连续性。

**实现方式（纯软件层）**：FakeBleCentral 注入 Opus 帧 -> coordinator 进入 wechat_input_method 模式 -> `WasapiRenderSink` 用 `TimedFakeSink`（core_tests.cc:377）接收 PCM -> 比对预期 PCM。

**断言**：
- 解码正确性：TimedFakeSink 收到的 PCM 与「源 PCM 经 Opus 编解码后的参考 PCM」做频域相关性/SNR（Opus 有损，不精确比对）。
- 首帧延迟：测量首帧 PCM 到达时间 vs `button_down`（复用 `LogWechatLatency` 现有埋点）。
- 连续性：无断流、无爆音（峰值检测）。

**L4 真机层补充**：真实 CABLE + WASAPI loopback 抓取工具（见 5.5）。

### 5.4 L3 固件真链路联调（新建，需固件改造）

**目标**：验证「ES8311 采集替代为回放 PCM -> Opus 编码 -> BLE 无线 -> 桌面端」全链路保真度，并复现真实无线环境下的 ASR/微信行为。

#### 5.4.1 固件改造（详见第 8 节）

1. storage 分区（`partitions_ota.csv:7`，0x610000，1984KB SPIFFS）挂载 esp_spiffs，存放测试 PCM 文件。
2. `audio_pipeline.c` audio_task 增加回放模式：回放时从 SPIFFS 读 PCM 填充 mono 数组，替代 `esp_codec_dev_read`（`:333`），保留 HPF+Opus 编码+BLE 发送链路不变。
3. control_rx 新增 `{"event":"test_playback","file":"test1.pcm"}` 设定回放源；`remote_button_down/up` 驱动录音启停（复用现成）。

#### 5.4.2 测试流程

```
1. 烧录改造固件 + SPIFFS PCM 镜像（spiffsgen.py 打包 -> esptool 烧 0x610000）
2. 桌面端连 BLE，开启 debug_audio_cache
3. 脚本下发 test_playback 选定语料 + remote_button_down
4. 固件回放 PCM -> Opus 编码 -> BLE 无线 -> 桌面端
5. 桌面端落盘 DebugAudio\*.ogg，ASR 识别，注入抓取程序
6. 断言：
   a. 落盘 ogg 与 L1 桌面端注入同一语料产生的 ogg 做链路保真比对（采样率/帧数/Opus 包序列一致）
   b. 抓取程序文本 vs 预期文本（CER）
   c. 日志状态机流转完整（recording->thinking->pending_confirmation->ready）
```

#### 5.4.3 微信模式真链路（L3 微信分支）

固件回放 PCM -> 桌面端 wechat_input_method 模式 -> 渲染到真实 CABLE Input -> WASAPI loopback 抓取 -> 比对预期 PCM（相关性）。验证真实无线+真实渲染端到端。

### 5.5 L4 微信输入法真机验证（半自动）

**目标**：验证微信输入法真的能从 CABLE 收到音频并识别出字（黑盒，无法全自动）。

**自动化部分**：
- WASAPI loopback 工具抓取 CABLE Input 渲染端 PCM，自动断言音频层正确（延迟、连续性、电平）。
- `auto_switch_default_recording_device` 切换行为日志断言（`default_audio_device_controller.cc`）。

**人工抽检清单**（关键用例）：
- [ ] 长按热键（Ctrl+Win）-> 微信语音弹框出现
- [ ] 说出测试语料 -> 微信候选框出现正确文字
- [ ] 松开 -> 文字上屏
- [ ] 短录音（<0.5s）过滤
- [ ] 连续多轮不串扰
- [ ] 首字延迟体感（<2s）

**OCR 辅助（可选）**：用截图+OCR 抓微信候选框做粗略断言，脆弱，仅作辅助不作为通过门禁。

## 6. 测试资产：语料与生成

### 6.1 语料设计

| 类别 | 用途 | 示例 |
|---|---|---|
| 常规短句 | 基线 CER | "今天天气不错" |
| 长句 | VAD/流式 | "把这段话翻译成英文…" |
| 数字/专有名词 | 易错边界 | "我的手机号是13800138000" |
| 中英混合 | 混排 | "用Python写一个hello world" |
| 静音/极短 | 边界丢弃 | <0.5s 应被丢弃 |
| 噪声环境 | 鲁棒性 | 带背景噪声录音 |
| DebugAudio 真实样本 | 回归 | 历史问题复现 |

每条语料：一个 `.pcm`（16kHz/16bit/mono）+ 一个 `.txt`（预期文本）。

### 6.2 生成方式

- **TTS 生成（主）**：Python `edge-tts` 生成中文 PCM（16kHz mono），内容可控、可断言、可重复。识别率偏高，作为基线。
- **真人录音（辅）**：用户录制少量真实语料，更贴近真实 ASR 难度。
- **DebugAudio 样本（回归）**：从 `%LOCALAPPDATA%\VoiceStick\DebugAudio\` 收集历史 ogg，转 PCM 作为回归语料。

### 6.3 语料存储

- 桌面端测试用：`tests/fixtures/corpus/`（ogg/pcm + txt）。
- 固件回放用：SPIFFS 镜像（PCM），由 `spiffsgen.py` 打包。

## 7. 自动化编排

新增 `scripts/e2e_test/` 目录：

| 脚本 | 职责 |
|---|---|
| `gen_corpus.py` | edge-tts 生成语料 PCM + txt |
| `run_l1_asr.py` | 驱动 L1：注入 ogg -> 等抓取程序结果 -> CER 断言 -> 报告 |
| `run_l2_wechat.py` | 驱动 L2：注入 Opus -> 比对 TimedFakeSink/loopback PCM -> 报告 |
| `run_l3_firmware.py` | 驱动 L3：BLE 下发 test_playback+remote_button -> 抓取+ogg 比对 -> 报告 |
| `capture_helper/` | 抓取程序源码（Win32） |
| `loopback_capture/` | WASAPI loopback 抓取工具源码 |
| `compare_ogg.py` | L3 链路保真比对（落盘 ogg vs L1 参考 ogg） |
| `cer.py` | 字错误率计算 |

编排器 `run_all.py` 串联 L0-L4，输出 HTML/JSON 报告。

## 8. 固件改造方案（L3 专属）

### 8.1 改造点

| 文件 | 改造 | 风险 |
|---|---|---|
| `firmware/main/CMakeLists.txt` | REQUIRES 增加 `esp_spiffs` | 低 |
| `firmware/components/audio_pipeline/CMakeLists.txt` | REQUIRES 增加 `esp_spiffs`（若回放在 pipeline 内） | 低 |
| `firmware/components/audio_pipeline/audio_pipeline.c` | audio_task 加 `s_playback_source` 开关 + SPIFFS 读 PCM 替代 `esp_codec_dev_read:333` | 中，需保证非回放模式零行为变化 |
| `firmware/main/main.c` | `ble_control_cb` 新增 `test_playback` 命令解析（`:682-789`） | 低 |

### 8.2 回放实现要点

- 回放模式开关默认关闭，仅 `test_playback` 命令激活，正常使用零影响。
- PCM 文件格式：16kHz/16bit/mono，与 `esp_codec_dev_read` 输出对齐（注意 codec 读的是立体声，回放需填 stereo 数组或调整单声道分支，参考 `audio_pipeline.c:338-340` 立体声转单声道逻辑）。
- 文件读完循环或静音填充，由 `remote_button_up` 停止。
- session_id/seq/flags/START/END 由现有链路处理，无需改动（`audio_pipeline.c:355,497`）。

### 8.3 烧录流程

1. `idf.py build` 编译改造固件。
2. `spiffsgen.py` 打包测试 PCM 为 SPIFFS 镜像。
3. `esptool` 烧录固件 + SPIFFS 镜像到 0x610000（COM17）。
4. 真机验证回放。

> 遵循 `sticks3-flash-ota` / `usb-jtag-flash-log` skill 流程。

### 8.4 TDD 约束

固件无单测，采用「行为契约 TDD」：
1. 先在 L1 定义「固件回放应产出与桌面注入一致的 ogg」契约（参考 ogg）。
2. 改固件回放。
3. L3 真机跑，比对落盘 ogg 与参考 ogg 一致 = 固件回放正确。
4. 非回放模式回归：确认正常录音行为不变（手动+日志）。

## 9. 桌面端改造与新增工具

| 项 | 类型 | 说明 |
|---|---|---|
| 抓取程序 `capture_helper` | 新增 exe | Win32 TOPMOST 窗口 + Edit，拦截 Return 提交文本到文件，供脚本断言 |
| WASAPI loopback 工具 | 新增 exe | 抓 CABLE Input 渲染端 PCM，L4 微信音频层断言 |
| `voicestick_integration_tests` | 新增测试目标 | L1/L2 真实 ASR 集成测试，链接 voicestick_core + 真实 asr_client |
| 命令行配置覆盖 | 可选新增 | `--config-override` 或环境变量切换 provider/output.target，免去改 config.toml 重启 |
| `debug_audio_cache` 测试态 | 配置 | 测试配置模板默认开启 |

## 10. 需要的配套支持（向用户索取）

1. **火山 ASR API key**：写入 `%APPDATA%\VoiceStick\config.toml`（`asr_provider="volcengine"` + `volcengine_api_key` + 对应 `X-Api-Resource-Id`）。
2. **固件烧录配合**：COM17 串口可用；改造固件 + SPIFFS 镜像烧录需在方案落地 L3 时配合执行（我可生成命令，你确认执行或授权我执行）。
3. **测试语料真人录音（可选）**：少量真实语料提升 ASR 测试真实度，TTS 生成为主则无需。
4. **测试运行环境**：运行 L1/L3 时抓取程序需保持前台置顶，测试期间避免抢占焦点；关闭其他蓝牙设备干扰。
5. **微信输入法热键确认**：确认当前 `config.toml` 的 `[wechat_input_method].hotkey` 与系统微信输入法语音快捷键一致（默认 `ctrl+win`）。

## 11. 实施里程碑

| 里程碑 | 内容 | 产出 |
|---|---|---|
| M1 | 桌面端基础设施：抓取程序 + loopback 工具 + 语料生成 | capture_helper.exe、loopback_capture.exe、gen_corpus.py、语料库 |
| M2 | L1 ASR 集成测试 | voicestick_integration_tests、run_l1_asr.py、CER 报告 |
| M3 | L2 微信渲染测试 | run_l2_wechat.py、TimedFakeSink 比对 |
| M4 | L3 固件改造 + 真链路联调 | 改造固件、SPIFFS 镜像、run_l3_firmware.py、链路保真报告 |
| M5 | L4 微信真机 + 人工清单 | loopback 断言、人工抽检清单 |
| M6 | 自动化编排 + 文档收尾 | run_all.py、HTML 报告、方案定稿 |

每个里程碑遵循 disciplined-execution：先写失败测试/契约，再实现，再验证，证据驱动。

## 12. 风险与对策

| 风险 | 对策 |
|---|---|
| 火山 ASR 识别波动致 CER 不稳定 | 多次跑取中位数；阈值留余量；区分「服务波动」与「链路 bug」用调试 ogg 离线重放对照 |
| BLE 无线丢包致 L3 ogg 与参考不一致 | 先比对帧序列结构（session/seq/flags），再比对内容；允许少量丢包，统计丢包率 |
| 固件回放改造引入正常录音回归 | 回放开关默认关；非回放路径零改动；真机回归正常录音 |
| 微信候选框 OCR 脆弱 | OCR 仅辅助不门禁；人工抽检为主 |
| storage 分区 1984KB 限制语料时长 | PCM 16kHz/16bit ≈ 32KB/s，1984KB ≈ 62s，单条语料足够；多条轮换烧录 |
| 抓取程序焦点被抢 | TOPMOST + 测试期间独占；脚本前检查前台窗口类名 |
| CABLE loopback 抓的是混音含系统声 | 测试期间静音其他音频源；或用独占模式抓取 |

## 13. 待实现时确认的细节

- `voicestick_core` 是否已暴露足够接口让集成测试注入 Opus 帧到 coordinator（需确认 BleCentral 接口与 coordinator 的帧投递入口）。
- 火山 ASR 的 `X-Api-Resource-Id` 当前配置值（`volcengine-asr.md:22-30`）。
- 抓取程序是否需要支持「指定窗口注入」而非前台（当前 InputInjector 只注入前台，抓取程序须前台）。
- 是否新增命令行配置覆盖（影响 L1/L2 切换 provider 的便捷度）。
