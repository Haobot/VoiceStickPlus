# 微信输入法模式首字延迟自动化测试与优化方案

## 一、背景与症状

用户对比两种激活方式：

| 方式 | 链路 | 观感 |
|---|---|---|
| 微信输入法快捷键（Ctrl+Win） | 电脑麦克风 -> 微信 ASR | 首字快、流畅 |
| 按下 VoiceStick 设备录音键 | 设备 mic -> 固件 Opus -> BLE -> 桌面端解码 -> WASAPI 虚拟麦克风(VB-CABLE) -> 微信 ASR | 首字延迟 1-2 秒，松开后微信仍在识别（"大缓存"感） |

要求：结合已有 `.ogg` 音频（`%APPDATA%\VoiceStick`，约 60 个真实录音）构建**全自动化测试与优化流程**，多维度提升首字响应速度与识别流畅度。

## 二、真机证据与瓶颈定位（已诊断，非猜测）

读 `%LOCALAPPDATA%\VoiceStick\VoiceStickApp.log` 的 `LogWechatLatency` 诊断点，正常长按 session（VS-D010，对应落盘 ogg）：

| 阶段（button_down 到达桌面端为锚点 +Nms） | 耗时 |
|---|---|
| -> auto_switch 设备切换完成 | 12-18ms |
| -> renderer.Start（WASAPI 初始化）完成 | +22-37ms |
| -> 首帧 Opus 解码入 ring | +3ms（热）/ +192-249ms（冷） |
| -> SendDown（微信弹框） | +34-49ms |
| **桌面端总计 button_down -> SendDown** | **74-296ms** |

**核心结论：桌面端可控链路仅 74-296ms，绝非 1-2 秒主因。**

- 首帧双峰（3ms 热 / 249ms 冷）印证固件 `init_codec` 冷热差异（见记忆 `wechat-first-char-latency-rootcause`，已决策不改固件）。
- auto_switch 12-18ms、renderer.Start 22-37ms 都是 WASAPI/COM 调用，难大幅压缩。
- 1-2 秒差额在 SendDown **之后**：VB-CABLE 虚拟声卡缓冲（疑似主因）+ 微信 ASR 黑盒缓冲 + 可能 ring buffer 积压（解释"松开后还在识别"）。

## 三、可行性分析

### 可全自动测/优化（.ogg 重放 + 现有测试设施）
现有 `FakeWasapiRenderSink`、`RenderPump`、`PcmRingBuffer`、`AudioOpusDecoder`、`FakeWechatInputMethodHotkey` 均可复用扩展。覆盖：
- 桌面端链路延迟与 underrun（Opus 解码 -> ring -> PumpOnce）
- `buffer_duration_ms` / `ring_capacity` 参数帕累托
- ring 积压与 drain 行为（"松开后还在识别"症状）
- ASR 链路首字/整段延迟（重放 .ogg 给腾讯 ASR WebSocket）

### 需真实 VB-CABLE（半自动）
端到端 loopback：重放 -> WASAPI render(CABLE Input) -> VB-CABLE 缓冲 -> CABLE Output capture -> 时间戳比对，量化 VB-CABLE 缓冲。**定位 1-2 秒主因的关键测试**。

### 不可自动测（黑盒/真机）
- 微信输入法出字延迟（第三方黑盒，需 UI 自动化/OCR，不精确）
- 固件 `init_codec` 冷启动、BLE 传输（真机，已决策不改）

### .ogg 格式确认
前 64 字节确认是标准 OggOpus：`OggS` magic + `OpusHead`（version=1, channels=1, preskip=312, sample_rate=16000）。由自家 `OggOpusMuxer` 产出，格式简单（每页一 Opus packet ≤255 字节），demuxer 易写。构建已 vendored libopus（`third_party/opus`），无 opusfile/ffmpeg 依赖，自写 demuxer。

## 四、方案总览（7 阶段，TDD 驱动）

```
阶段1 OggOpus 重放核心 ──┐
阶段2 桌面端链路延迟测量 ─┼─> 阶段6 桌面端有限优化 ─> 阶段7 评估回归
阶段3 ring 积压诊断 ─────┘
阶段4 ASR 链路延迟基线 ──────────────────────────────┘
阶段5 VB-CABLE loopback（定位主因，需真机 VB-CABLE）
```

## 五、各阶段详述与 TDD 清单

### 阶段1：OggOpus 重放核心
**新增** `src/ogg_opus_demuxer.h/.cc`（加入 `voicestick_core`），`src/audio_replayer.h/.cc`。

`OggOpusDemuxer`：解析 OggOpus 字节流 -> Opus packet 列表 + 元信息（sample_rate, channels, preskip, granule）。

TDD 测试（`core_tests.cc`）：
- `TestOggOpusDemuxerParsesOpusHead`：解析自家 muxer 产出的头，验证 preskip=312 / sample_rate=16000 / channels=1。
- `TestOggOpusDemuxerExtractsAudioPackets`：提取 N 个 Opus packet，数量与 muxer 输入一致。
- `TestOggOpusDemuxerRoundTrip`：muxer 编码已知 packet 序列 -> demuxer 解析 -> packet 逐字节一致。
- `TestOggOpusDemuxerHandlesEos`：末页 header_type=0x04 正确识别结束。
- `TestOggOpusDemuxerRejectsBadMagic`：非 OggS 流返回错误。

`AudioReplayer`：按 20ms 时序调度 Opus packet 喂给"解码->ring"链路（可变速率模拟抖动/突发）。
- `TestReplayerSchedulesFramesAt20msCadence`。

### 阶段2：桌面端链路延迟测量
**扩展** `FakeWasapiRenderSink` 为计量版本（或新增 `InstrumentedWasapiRenderSink`）：记录每次 `PumpOnce` 的时间戳、padding、消费帧数、underrun（ring 空补静音）。

TDD 测试：
- `TestInstrumentedSinkMeasuresWriteToConsumeLatency`：帧写入 ring -> sink 消费的时间差。
- `TestInstrumentedSinkCountsUnderrun`：ring 空时补静音计入 underrun。
- `TestWechatPipelineFirstFrameLatencyWithOggReplay`：重放真实 .ogg，测首帧延迟（< 阈值）。
- `TestWechatPipelineBufferDurationPareto`：buffer_duration_ms ∈ {10,20,50,100} 的延迟-underrun 权衡表。

### 阶段3：ring 积压诊断（"松开后还在识别"）
重放模拟生产/消费失配（突发多帧、消费瞬时停顿），测 ring 积压量与 drain 时长。
- `TestRingBacklogAccumulatesOnBurst`：突发写入后 ring Available 上升。
- `TestRingBacklogDrainsAfterStop`：Stop 后 drain 残余数据时长可量化。
- 评估优化：积压超阈值时限速写入或 Stop 时限时 drain（防"松开后还在识别"）。

### 阶段4：ASR 链路延迟基线
重放 `.ogg` 给腾讯 ASR WebSocket（`asr_client_tencent`），测首字/整段延迟、识别准确率。对比不同 .ogg 样本。可独立 C++ 测试或复用 `scripts/probe_asr_websocket_ping.py` 模式。
- 需真实腾讯凭据（config.toml 已有）。
- 量化 ASR 缓冲是否贡献 1-2 秒。

### 阶段5：VB-CABLE loopback（定位主因，需真机）
重放 .ogg -> 真实 `WasapiVirtualMicRenderer`（CABLE Input）-> VB-CABLE 缓冲 -> CABLE Output WASAPI capture -> 时间戳比对。
- 量化 VB-CABLE 端到端音频缓冲延迟（写入 ring 到 capture 端读到）。
- 确认/排除 VB-CABLE 为 1-2 秒主因。
- 需真实 VB-CABLE 驱动安装，半自动。

### 阶段6：桌面端有限优化
基于阶段1/2/3 测量，TDD 改造：
- `buffer_duration_ms` 50->20ms（阶段2 帕累托验证不引入 underrun 后）。
- auto_switch 与 renderer.Start 并行化（省 12-18ms，需保 SendDown 前两者都完成）。
- 真机 `LogWechatLatency` 日志对比优化前后。

### 阶段7：自动化评估与回归
汇总各阶段测量，输出延迟分解报告 + 基线对比 + 回归保护。

## 六、诚实预期与边界

| 维度 | 预期 |
|---|---|
| 桌面端可控链路优化 | 压低 ~30-50ms（buffer 30ms + auto_switch 并行 12-18ms） |
| "松开后还在识别" | ring 积压消除可缓解；若主因在微信 ASR 则属黑盒 |
| 1-2 秒主因定位 | 阶段5 loopback 量化 VB-CABLE 缓冲；若为主因，建议调 VB-CABLE latency 配置（非代码） |
| 微信 ASR 黑盒 | 不可控，接受或提示用户 |

**自动化测试核心价值**：定位瓶颈归属（桌面端已证非主因 -> 下游 VB-CABLE/微信）+ 回归保护 + 有限优化，而非凭空消除不可控的下游延迟。

## 七、实施顺序与验证

按阶段顺序推进，每阶段 TDD（红->绿->重构）+ `build_win.bat` 构建 + `ctest` 验证。每阶段测试通过后按记忆 `feedback-windows-test-msi-commit` 提交（`git add -f desktop/windows`）。

阶段5 需真机 VB-CABLE，标记为半自动，最后执行。
