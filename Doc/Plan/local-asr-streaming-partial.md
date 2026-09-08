# 本地语音识别流式 Partial 输出

状态：已实现（feat/voice-recognition-option 分支）
日期：2026-09-09

## 背景与问题

本地语音识别（SenseVoice-Small int8，经 sherpa-onnx）当前是"整段一次性"识别：
`LocalAsrClient::SendOggOpusChunk` 只累积 Ogg 字节，`is_last=true` 才在 worker 线程做
ParseOggOpus → Opus 全量解码 → SenseVoice 离线推理，只触发 `on_final`。录音全程 UI 只显示
"Listening..."，而云端 ASR（火山 WebSocket）边说边出 partial 文本。体验差距明显。

## 目标

录音期间本地识别也持续产出 partial 文本，复用云端已有的 partial 显示路径（悬浮窗
`ShowPartial`、字幕模式、StickS3 设备屏 `ui_state("thinking", text)`），体验接近云端。

## 方案对比

1. **滚动全量重解码（选定，伪流式）**：录音期间周期性对"到目前为止的全部音频"跑一次
   SenseVoice 离线推理触发 `on_partial`；`is_last` 做最终推理触发 `on_final`。
   - 优点：零 UI/协调器改动（接口回调早已接线）；文本始终带上文，语义连贯；
     SenseVoice 准确率/标点/多语言能力完整保留。
   - 代价：单次推理耗时随时长线性增长（int8 2 线程 RTF ~0.1 量级，20s 音频一次
     ~1-2s），长录音时 partial 频率自然下降——实时流场景音频增长速率与推理速率
     同为实时，追赶式调度可保证不丢尾。
2. **换流式模型（Zipformer/Paraformer streaming）**：需引入第二个模型与下载链路，
     中英混说、标点、准确率全面弱于 SenseVoice-Small。否决。
3. **VAD 分段 + 段级上屏**：按静音切段逐段识别。段间无上下文导致跨段文本不连贯、
     准确率下降，且需额外 VAD 模型。否决。

## 设计

### 引擎抽象（可测性前提）

sherpa-onnx C API 是全局函数，无法在无模型环境下驱动调度逻辑。抽出推理引擎接口：

```cpp
// local_asr_client_win.h
class SenseVoiceEngine {
 public:
  virtual ~SenseVoiceEngine() = default;
  // 整段 16kHz 单声道 PCM16 做一次离线推理，返回 UTF-8 文本。仅在 worker 线程调用。
  virtual std::string Decode(std::span<const std::int16_t> samples) = 0;
};
```

- 生产实现 `SherpaSenseVoiceEngine`：包 recognizer 创建（原 `Start` 内逻辑）与
  create stream → accept → decode → get result → destroy 循环。
- `LocalAsrClient` 构造函数追加可选 `engine_override`；注入时跳过模型目录校验。
  测试用 fake 引擎驱动调度（无模型环境 TDD）。

### 流式调度（LocalAsrClient::Impl::WorkerMain）

锁保护状态：`ogg_buffer_`（追加）、`ogg_version_`（递增）、`is_last_received_`。
worker 私有状态：`packets_decoded_`、`pcm_`（累积 PCM16）、`last_text_`、
`last_decoded_samples_`。

```
loop:
  等待直到：shutdown / is_last_received_ / 到达 next_decode_time_ 且有新数据
  is_last          → 最终解码（立即，不等周期）
  周期到且有新数据 → partial 解码
```

- **周期**：`kPartialIntervalMs = 600`。首个 chunk 到达即解码（协调器本就缓冲 0.5s
  才开始发，首个 partial ~0.6s 内出现，与云端首 partial 量级一致）；此后
  `next_decode_time_ = 上次解码完成 + 600ms`（解码超期则完成后立即追赶下一轮）。
- **增量 Opus 解码**：每次解码先全量 `ParseOggOpus`（解析开销可忽略），再从
  `packets_decoded_` 起只解码新增 packet 追加 `pcm_`，避免音频被重复 Opus 解码。
- **空 partial 不上报**：SenseVoice 对起始静音可能输出空文本，为保持 "Listening..."
  显示，partial 文本为空时跳过回调。
- **final 复用**：`is_last` 时若自上次推理后新增样本为 0（松键后无新音频），直接以
  `last_text_` 触发 `on_final`，松键即出结果，省一次推理。
- **错误语义**：partial 阶段解析/解码失败静默跳过（不打断会话，与云端网络抖动不
  杀会话一致）；final 阶段失败 `on_error`（沿用现文案）。
- **Cancel/Start**：`Cancel()` 置 cancelled、清锁内状态；worker 在锁内检测到
  cancelled 清自身增量状态（丢弃积压）。`Start()` 重置 cancelled 与锁内会话态。
  推理完成后回调前检查 cancelled，取消则丢弃。

### 不改动的部分

协调器、UI、设备协议、配置文件全部零改动——`WireAsrClientCallbacks` /
`ConfigureSubtitleAsrCallbacks`（字幕会话经 `asr_factory_` 同样拿到 LocalAsrClient）
对 `on_partial` 的消费早已就绪。partial 间隔不做成配置项（YAGNI，常量即可）。

## 测试（core_tests.cc，fake 引擎驱动）

1. `TestLocalAsrClientEmitsPartialWhileStreaming`：分块喂音频，断言 ≥2 个 partial
   且输入样本数单调增，`is_last` 后 final 非空。
2. `TestLocalAsrClientPartialThrottled`：连发 5 块后立即采样 decode 次数 ≤2
   （无节流会=5），验证不逐块解码。
3. `TestLocalAsrClientFinalReusesDecodeWhenNoNewAudio`：is_last 无新音频时
   decode 次数不增、final 文本等于最后 partial。
4. `TestLocalAsrClientSkipsEmptyPartial`：fake 返回空文本时不触发 on_partial，
   final 仍正常。
5. `TestLocalAsrClientCancelStopsPartial`：Cancel 后不再有任何回调。
6. `TestLocalAsrClientSenseVoiceSmoke`（真模型，不在位 SKIP）：流式分块喂，
   断言收到 partial 且 final 非空。

## 权衡与已知限制

- 长录音（>30s）时单次推理耗时上升，partial 间隔被动拉长；主流程（按键语音输入，
  通常 <15s）不受影响。
- 非流式模型对增长音频的输出非单调（尾部可能被后续 partial 修正），与云端 partial
  行为一致，UI 本就按"最新文本覆盖"渲染。
