# 精修链路延迟优化（B 方案先行 + A 方案本地小模型实测）实施方案

> 状态：已定案待实施（2026-10-14 定案；实施者另一 AI 会话，本文自包含；未核销）

- 日期：2026-10-14
- 关联：`Doc/Expe/voice-input-latency-root-cause-chain-2026-10-12.md`（BLE/倒计时段延迟已修）、`Doc/Plan/local-text-refinement.md`（Windows 本地精修 spike：小模型必须 few-shot、指令式 prompt 在 ≤1.7B 上失败）

## 0. 给实施者的环境须知（先读）

- 工作树当前有**未提交的 RNNoise 相关改动**（`VoiceStickCoordinator.swift`、`AppConfig.swift`、`Package.swift`、新增 `Sources/CRnnoise/` 等），与本任务无关。**不得 revert/覆盖这些改动**，一律用增量 Edit 叠加。
- macOS 构建：`cd desktop/macos && VOICESTICK_ARCHS=arm64 swift build`（本机 CLT-only，必须带 `VOICESTICK_ARCHS=arm64`）。测试：`swift run VoiceStickTests`（executable runner，非 XCTest）。
- 验证真机行为必须 `open` 启动 app（CLI 直启会因 TCC 归因静默失去蓝牙）；日志在 `~/Library/Application Support/VoiceStick/VoiceStickApp.log`。
- 用户当前精修配置（`~/Library/Application Support/VoiceStick/config.toml`）：`llm_base_url=https://api.deepseek.com`、`llm_model=deepseek-v4-flash`、`refine_enabled=true`、`refine_prompt=""`（走默认长 prompt）、`llm_disable_thinking=true`、无热词。

## 1. 背景：实测数据与根因

用户感知「识别很快、精修很慢」。从本机 `VoiceStickApp.log` 提取 2026-10-14 的会话，「末帧音频送达 ASR」→「进入待粘贴（=精修完成）」耗时：

| 说话时长 | 文本长度 | ASR收尾+精修耗时 |
|---|---|---|
| 7.7s | 77B | 2.7s |
| 6.9s | 89B | 3.2s |
| 14.8s | 167B | 13.9s |
| 7.8s | 96B | 4.6s |
| 18.9s | 136B | **29.1s** |
| 33.5s | 351B | 23.0s |
| 11.7s | 92B | 12.1s |
| 24.0s | 234B | 16.5s |

之后还有固定 +0.42s 浮窗确认倒计时（`OverlayController.showFinal`，本轮不动）。

根因（代码实证）：

1. **串行瀑布**：`VoiceStickCoordinator.finishWithFinalText`（`desktop/macos/Sources/VoiceStickCore/VoiceStickCoordinator.swift:1828`）在 ASR final 到达后才启动 `refiner.refineStream`，说话期间精修完全闲置。
2. **流式只用于展示**：SSE token 仅更新浮窗（60ms 节流），注入文本框要等 `onComplete` 全文生成完；输出 token 数 ≈ 输入长度，长文本线性变慢。
3. **端点抖动**：136B→29s 与 351B→23s 并存，说明 DeepSeek 端 TTFT/排队抖动主导，不是生成长度问题。
4. **失败放大**：SSE 失败回退非流式重发一整轮（`LLMRefinementClient.swift:60`），最坏 8s 空闲超时 + 再一轮完整时延。

## 2. B 方案：快速回退与保护（macOS 先行，Windows 对齐随行）

目标：**砍掉长尾**——宁可快速粘贴 ASR 原文，不让用户等 29s。全部改动在 macOS 两个文件 + 测试。

### B1. 精修打点日志（先做，后续所有优化的对照基线）

在 `VoiceStickCoordinator.finishWithFinalText` 的 refine 分支加三条 NSLog（进 `VoiceStickApp.log`，与其他打点同风格）：

- 启动时：`refine begin text_len=<utf8字节数>`；
- `onToken` 首次回调：`refine first token ms=<距begin毫秒>`；
- `onComplete`：`refine complete ok=<0/1> ms=<总耗时> out_len=<结果utf8字节数> fallback=<none|sse_to_blocking|budget|hotword_guard>`；
- 预算回退时：`refine budget exceeded after <ms>ms; pasting original text_len=<N>`。

计时用 `Date()` 局部变量（begin 时刻），`onToken`/`onComplete` 闭包捕获。注意闭包在 URLSession 回调线程，日志线程安全（NSLog 本身安全），耗时计算与写日志都在回调线程完成、不进主线程即可。

### B2. 精修预算超时回退原文（核心改动）

在协调器层（**不是** client 层——要覆盖 SSE→非流式内部回退的总时长）加预算定时器：

- `finishWithFinalText` 启动 `refineStream` 后，arm 一个主线程 `Timer`（参照 `audioEndTimeoutTimer` 模式）：
  - **首 token 预算** `kRefineFirstTokenBudget = 2.5s`：到时仍无 token → 判定端点卡死。
  - **总预算** `kRefineTotalBudget = max(5.0s, 0.025s × text.utf8.count)`：按文本长度伸缩，短文本 5s 封顶，长文本给足生成时间（351B ≈ 8.8s）。
  - 两个定时器合一即可：总预算定时器 armed 全程；首 token 预算单独一个，`onToken` 首次到达时 invalidate。
- 任一预算触发：`token.cancel()`（复用既有 `RefineCancelToken`，迟到的 `onComplete` 会被 `!token.isCancelled` 守卫丢弃）、invalidate 两个定时器、打 B1 的 budget 日志、`finishWithRefinedText(text)`（**原文**）走完既有粘贴链路。
- 定时器清理点：`onComplete` 正常路径、`cancelStreamingRefinement()`（新会话/取消/配置热更新都会经过，在这里一并 invalidate，防悬挂定时器误杀下一次会话的精修）。
- 常量定义在 `VoiceStickCoordinator` 顶部既有常量区（`audioEndTimeout` 附近），先硬编码；是否提为 config 字段留作后续（提 config 要联动 `ConfigFileModels.swift` + `config.example.toml` + `Doc/Ref/desktop-config.md` + Windows，本轮不做）。

### B3. 短文本跳过精修

`finishWithFinalText` 的 refine 分支前置判断：`text` 去掉空白后 < `kRefineMinChars = 12`（字符数，非字节）直接走 `finishWithRefinedText(text)`。实测 12B 短句精修也要 1.2s+，收益极低。打一行 `refine skipped (short text) len=N` 便于归因。

### B4. 请求体小优化（`LLMRefinementClient.makeRequest`）

- 加 `"max_tokens"`：`max(64, text.utf8.count * 2)`（中文约 1 token/字，2× utf8 字节数足够覆盖且防跑题拖长输出）。
- `llmDisableThinking` 时除既有 `enable_thinking`/`chat_template_kwargs` 外，**再发 `"think": false`**（Ollama 约定的关思考字段；其他端点忽略未知字段，无副作用——这是 A 方案本地 qwen3 不思考直出的前提）。

### B5.（可选，本轮先不做）精简默认 system prompt

默认 prompt ~500 字（`LLMRefinementClient.swift:158`）逐字对齐 Windows `BuildRefinePrompt`，改动需双端同步且有质量回归风险。DeepSeek 有 context caching，重复 system prompt 的 prefill 代价低。**本轮不动**，待 B1 打点证明 prefill 是瓶颈再议。

### B6. 测试（`desktop/macos/Tests/VoiceStickTests/`）

`CoordinatorFsmTests.swift` 已有 `FakeRefiner`（约 :121，立即 complete）。扩展为可编程 fake（延迟 complete / 永不 complete / 记录调用次数），新增用例：

1. 预算超时 → 粘贴原文（fake 永不 complete，推进主 RunLoop 超过预算，断言 `FakeInput.pastedText == 原文`）；
2. 预算内完成 → 粘贴精修结果（现行为回归）；
3. 短文本 → fake 未被调用，直接粘贴原文；
4. 超时后迟到的 complete 被丢弃（不二次粘贴）。

构建验证：`VOICESTICK_ARCHS=arm64 swift build && swift run VoiceStickTests`（基线 768/768，新增后应全绿）。

### B7. Windows 对齐（本机无法构建，标 TODO）

`desktop/windows/src/voice_stick_coordinator.cc` `FinishWithFinalText`（:2386，RefineStream 在 :2720 附近）镜像 B2/B3 同一语义与同名常量；`llm_refinement_client.cc` 镜像 B4。本机只能对无 Win32 依赖的文件 `clang++ -fsyntax-only`，协调器不行——**在 Windows 机器上 `build_win.bat` + ctest 验证**（可用 build-windows skill 的流程）。macOS 先行落地不阻塞。

## 3. A 方案：本地小模型精修实测（B 落地后做）

目标：用本地 OpenAI 兼容端点替代 DeepSeek，量化 TTFT/总时延/质量，决定是否切默认。本机 M4 Pro 24GB，无 Ollama（已确认未安装）；app `Info.plist` 已配 `NSAllowsLocalNetworking=true`，**http://127.0.0.1 直连可用，无需改 ATS**。

### A1. 端点搭建

安装 Ollama（`brew install --cask ollama` 或官网 pkg），`ollama serve` 常驻。候选模型（**避开默认开启 thinking 的模型**，或依赖 B4 的 `"think": false`）：

| 候选 | 拉取命令 | 备注 |
|---|---|---|
| qwen2.5:3b-instruct | `ollama pull qwen2.5:3b-instruct` | 非思考型，首选基线 |
| qwen3:4b-instruct-2507 | `ollama pull qwen3:4b-instruct-2507` | 非思考型，质量上限更高 |
| qwen3:1.7b | `ollama pull qwen3:1.7b` | 思考型，验证 B4 的 think:false 是否生效 |

### A2. 基准脚本 `scripts/probe_refine_latency.py`（新建）

参照 `scripts/probe_asr_websocket_ping.py` / `probe_hotword_extraction.py` 的风格。功能：

- 从命令行读 `--base-url`、`--model`、`--api-key`（可空）、`--runs N`（默认 8）；
- 用与 app **完全一致**的请求体（system prompt 复刻 `LLMRefinementClient.buildPrompt` 默认形态 + `stream:true` + `temperature:0` + B4 字段）；
- 测试文本 4 档：取自真实日志长度分布（约 12B / 80B / 170B / 350B 中文口语文本，内含"嗯/那个/就是"等口水词与一处热词）；
- 每档 × N 次，输出：TTFT p50/p95、总耗时 p50/p95、tok/s、以及精修结果是否保热词/是否丢内容（简单子串与长度比启发式，打印全文供人工判读）；
- 对照组跑 DeepSeek 现配置（key 从本机 config.toml 读，**脚本只读不写、不回显 key**）。

### A3. 真机切换验证

改 `~/Library/Application Support/VoiceStick/config.toml`（改完重启 app）：

```toml
llm_base_url = "http://127.0.0.1:11434/v1"
llm_api_key = "ollama"          # 占位即可：makeRequest 要求非空，Ollama 忽略该头
llm_model = "qwen2.5:3b-instruct"
```

真机说 10+ 条（短/中/长各几条），用 B1 的打点从 `VoiceStickApp.log` 读 `refine begin/first token/complete` 分段耗时。

**验收线**（未达则保留云端配置，记录数据）：

- 延迟：≤200B 文本 refine 总耗时 p50 ≤ 1.5s、p95 ≤ 3s（对比 DeepSeek 现 p50 ≈ 4s、长尾 29s）；
- 质量：人工抽检 20 条，不比云端精修差（无丢内容、无改写原意、标点/口水词处理达标）。

### A4. 质量不达标时的已知路径（不重造轮子）

Windows spike（`Doc/Plan/local-text-refinement.md` §2）已证明：**≤1.7B 模型对指令式 prompt 无效，必须 few-shot 重写形态**；以及需要守卫（子序列校验+删除片段校验）防小模型丢内容。若 A3 质量不达标，按优先级：

1. 先把 `refine_prompt` 配置成 few-shot 形态（零代码）重测；
2. 仍不达标 → 结论是「本地小模型需守卫链路」，评估是否移植 Windows `text_refiner`（L1 规则 + L3 守卫，纯函数）到 macOS——那是独立一轮工作，另立方案，不在本方案范围。

## 4. 里程碑与交付顺序

1. **M1（B1+B2+B3+B4+测试）**：macOS 构建绿、VoiceStickTests 全绿、真机 3 条语音验证打点与预算回退生效（人为把 `llm_base_url` 指向黑洞地址如 `http://127.0.0.1:9` 可触发 budget 回退路径）。预期效果：长尾从 29s 压到 ≤5s（短文本）/按长度封顶。
2. **M2（A1+A2+A3）**：基准数据表 + 真机打点对比 + 切/不切的结论。
3. **M3（B7）**：Windows 镜像 + Windows 机构建验证。

## 5. 风险与回滚

- 预算回退的语义变化：超时后用户拿到未精修原文——这是设计意图（宁快勿慢），常量在协调器顶部一处可回调。
- `max_tokens` 设小可能截断长文本精修结果：2× utf8 字节数对中文有 ~2× 余量；若日志出现 `finish_reason=length`（可在 B1 的 complete 日志顺带记录）再上调。
- `"think": false` 对 OpenAI/DeepSeek/DashScope 均为未知字段被忽略；若某端点严格校验报错，按端点收窄（届时再加配置门控）。
- 全部改动在桌面端 Swift/配置层，无协议/固件变更，回滚 = git revert。
