# 本地优先语音栈方案（ASR / 精修 / 语音改字全面本地小模型化）

- 日期：2026-10-28（数据支撑见 `Doc/Test/local-fullstack-eval-2026-10-28.md`）
- 状态：方案（评测完成，未实现；本文是迁移路线图与架构定论）

## 1. 结论

**GO（分阶段）。** 全本地栈（SenseVoice int8 + 确定性清理 + 受限编辑指令 ×
qwen2.5:3b/1.5b）实测管线 CER 1.67%、零损伤、模型链路延迟 ≈110ms，与云端栈
（腾讯 0.89% + 云精修 2.5~12.8s）相比：**准确率差 0.78pp，最终文本产出快一个
量级，且隐私/断网/零成本**。「逐步迁移」按 §5 的 M1→M4 推进，每步可回退。

不可行的是**照搬云端形态**：整句重写式精修对 ≤8B 模型实测损伤 20%~100%
（含「我的→你的」语义反向），已被 27 组合数据判死。

## 2. 现状盘点（2026-10-28）

| 层 | macOS | Windows |
|---|---|---|
| ASR | 腾讯/火山/voicestick_cloud 云端流式（`ASRWebSocketClient`/`TencentASRClient`） | 同左 + **本地 SenseVoice int8 已落地**（`local_asr_client_win.cc`，`[local_asr] enabled`，模型随 `pack_local_models.py` 分发） |
| 文本精修 | 云端 LLM SSE（`LLMRefinementClient`） | 云端 + **本地 llama.cpp**（`local_refinement_client.cc`，Qwen3-1.7B-Q4_K_M + few-shot + `RefineResultSafe` 删除守卫） |
| 语音改字 | 确定性解析 + **ollama 对齐兜底已落地**（`VoiceTargetedAlign`，qwen2.5:1.5b，默认关） | 确定性解析同构；对齐未接 |
| 既有缺口 | 本地 ASR 无、本地精修无 | 精修守卫只放行删除 → **修不了错字**；对齐未接；C 层修复双端未落地 |

## 3. 目标架构

```text
设备音频（Opus）→ [RNNoise 可选] → 本地 ASR：SenseVoice int8 常驻引擎
                                        （双端同模型同参数：lang=auto, itn=1, 6 threads）
     ┌── 指令轮 → targeted 解析/匹配/规划（确定性，零改动）＋ 拼音预筛
     │             ＋ 本地对齐（qwen2.5，枚举约束，已落地）→ verified 选区执行
     └── 文本轮 → L1 det 确定性清理（填充词/叠字/CJK空格；规则库可扩，零 LLM）
                  L2 editform 受限编辑指令（本地 LLM）
                     模型只出「错词→热词原形 / 删除：片段」；
                     代码守卫执行：替换目标∈(热词∪纠错对∪上文)，错词/片段
                     必须真实存在于原文，≤N 条，大小写不敏感定位；
                     LLM 层输出「无」/超时/逃逸 → 原文直通（失败即回退）
                  L3（保留）RefineResultSafe 风格终验：净差异仅允许
                     删除片段＋已验证替换，否则回退 L1 结果
                        → 字幕/注入
```

### 3.1 每层职责与失败语义

| 层 | 引擎/模型 | 失败语义 | 实测背书 |
|---|---|---|---|
| ASR | SenseVoice int8（239MB） | 引擎失败→云端回退（混合模式）或报错（纯本地） | CER 2.22%，40ms/条，抖动 0 |
| 指令拦截 | 确定性（现网零改动） | 不命中→普通文本轮 | 现网已验证 |
| L1 det | 规则（text_refiner 家族升级） | 规则只删不改 → 无失败面 | 31 条零损伤；v1「这个API」教训→规则要过真实数据 |
| L2 editform | qwen2.5:3b（主）/1.5b（低配） | 无/超时/枚举逃逸→原文 | 1.67%，+3/−0，70ms |
| 语音改字对齐 | qwen2.5:1.5b（已落地） | NONE→回退上屏 | 94%/82ms（前轮） |

### 3.2 可靠性设计原则（全部有本轮或前轮数据背书）

1. **确定性优先**：封闭类问题（填充词、口吃、空格、选区编辑、指令解析）
   不进模型。
2. **LLM 只出指令、守卫代码执行**：三个任务（targeted 对齐、跨轮纠错、
   editform 精修）同构实证；候选/替换目标必须可溯源（热词、上文、原文子串），
   模型无法制造不存在的编辑对象。
3. **失败即回退，绝不静默变造**：每层失败输出=输入。
4. **损伤率是一级指标**：clean-damage>0 的形态不得上线（整句重写因此出局）。
5. **选型必须过 harness**：任务形态定律三次验证——qwen2.5:1.5b 对齐第一但
   精修不敌同家族 3b；qwen3 家族三任务两崩塌。

### 3.3 与现有模块的衔接点（文件级）

| 动作 | 位置 |
|---|---|
| macOS 本地 ASR | 新 `LocalAsrClient: ASRClient`（vendored sherpa-onnx C target，先例 COpus/CRnnoise）；`ASRClientFactory` 按 `[asr] provider=local` 分流 |
| macOS 本地精修 | `LocalAlignClient` 泛化为 `LocalLlmClient`（同 ollama 回环通道）；协调器 `finishWithFinalText` 在云端精修前按 `[refine] provider=local` 分流 |
| editform 守卫 | Core 纯逻辑（对齐 `VoiceTargetedEdit.swift`/`voice_targeted_edit.cc` 风格）：`RefineEditParser`＋`ApplyGuardedEdits`＋单测 |
| Windows 精修改造 | `local_refinement_client.cc` 增 editform 路径；`model_manifest.cc`/`pack_local_models.py` 模型清单 Qwen3-1.7B → Qwen2.5-3B/1.5B Q4_K_M（llama.cpp 运行时不变） |
| 热词源 | 现有热词飞轮（`asr_hotwords`/correction pairs）直接作为 editform 的替换目标白名单 |

## 4. 模型选型定论（本机 M4 Pro 实测）

| 任务 | 推荐 | 淘汰 | 依据 |
|---|---|---|---|
| 本地 ASR | **SenseVoice int8**（Windows 已在发） | whisper tiny/base/small（崩塌）；turbo（延迟×14 只换最差值） | CER 2.22%/40ms；turbo 2.33%/552ms；small 16% |
| 本地精修 | **qwen2.5:3b-instruct Q4**（主）+ 1.5b（低配档） | 整句重写形态（全部 9 模型）；qwen3 全家族；llama3.2:3b；gemma3:1b | editform：3b 1.67% +3/−0 / 1.5b 2.08% +1/−0 |
| 语音改字对齐 | qwen2.5:1.5b（已落地，不变） | — | 前轮 94%/86%，本轮不重测 |

低配档语义：1.5b 与 3b 共享 tokenizer 家族与 ollama/llama.cpp 运行时，
按内存/用户选择切换；两档延迟 44/70ms 均远低于预算。

## 5. 迁移路径（每步独立可验收、可回退）

- **M1 Windows 精修升级**（改动最小、收益立现）：editform 路径 + 模型换
  Qwen2.5-3B/1.5B GGUF + det v5 规则。验收：`local_model_eval` harness 双端
  同分；真机 damage=0。
- **M2 macOS 本地化补齐**：vendored SenseVoice（`provider=local`）+ ollama
  editform 精修（`provider=local`）。验收：同 harness + 真机断网可用。
- **M3 语音改字收口**：C 层 `isCommandFallback` 修复（双端，安全前置）+
  Windows 对齐同构。验收：`Doc/Test/targeted-local-align-acceptance` 七用例双端过。
- **M4 默认翻转**：`local_first=true`（云端变回退档），配置与 UI 双端同步。
  保留混合模式：本地 ASR 失败/置信异常 → 云端重试，用户可全局锁本地。

## 6. 风险与未决

1. 真实口音/长会话分布未覆盖——M1/M2 验收时用 DebugAudio 回放扩充语料
   （复用 `replay_*.py` 思路，本地引擎进程内回放）。
2. editform 增益依赖热词表；无热词用户当前只获得 det 层。后续可把
   correction-pairs 飞轮与上文词汇纳入替换白名单（跨轮纠错同源）。
3. 本地无流式 partial 的交互代价：以录音态 UI 补偿；若实测不可接受，
   备选 VAD 分段 + SenseVoice 增量解码（伪流式，后续 spike）。
4. Windows x86 延迟未在本轮量化（引擎已在产线服役，量级风险低）。
5. 模型分发：SenseVoice 已有离线包轨道；Qwen2.5 GGUF 走
   `model_manifest.cc` 清单更新 + COS models/（§`cos-distribution.md`）。
