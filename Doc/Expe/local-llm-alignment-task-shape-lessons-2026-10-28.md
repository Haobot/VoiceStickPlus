# 本地小模型工程：任务形态决定选型 + 候选约束式安全架构（2026-10-28 targeted 对齐实战）

- 日期：2026-10-28
- 相关文件：`desktop/macos/Sources/VoiceStickCore/VoiceTargetedAlign.swift`、
  `Sources/VoiceStickApp/LocalAlignClient.swift`、`scripts/local_align_bench/`
- 相关 commit：`b453bcc`（feat 主体）、`d7a6743`（补测）
- 配套：方案 `Doc/Plan/voice-edit-commands.md` §17.7；评测数据
  `Doc/Test/local-llm-target-alignment-eval-2026-10-28.md`（本文只留教训）
- 时效声明：模型名/分数/延迟为记录时点结论，新模型出来用 harness 一键重测。

## 教训 1：模型选型结论不可跨任务形态搬用（最重要）

同一批模型，**任务形态一变结论反转**：M0 跨轮纠错（`Doc/Plan/local-asr-accuracy-and-cross-turn-refinement.md`）
里 Qwen3-4B GO / 1.7B NO-GO；到了本任务（few-shot 多轮 + JSON-schema 枚举约束 +
结构化 reason/answer 输出），**qwen3 全家（1.7b/4b/8b）NONE 崩塌**（正例 18~71%），
胜者是 qwen2.5:1.5b（主集 94%、留出 80%、零危险误选、~80ms）；qwen2.5:3b 也崩塌。
qwen3:8b 靠规模回升到 71% 仍全面输给 1.5b，延迟 4.5 倍。**判据：换任务先跑 harness，
不引用旧 spike 结论**；「加参数量」不如「选对家族 + 对的输出形态」。

## 教训 2：重叠候选簇是全模型杀手——能在预筛层解决的不给模型

首版候选含「升旗/看升旗/旗」「北京/去北京/北京上」这类同位重叠变体，**所有模型**
（含 8b）都在词边界上犯错（选「看升旗」「多美食」「我今天」）。修复=预筛重叠簇内
只留与目标等长的候选，1.5b 正例 10/18→16/17。同理，**负例垃圾单词（等长 0.5 分的
「上/今/美」）用非等长阈值 0.9 在预筛层直接消灭**——4/7 负例根本不需要模型调用。
先修确定性层，再调 prompt。

## 教训 3：纯枚举直出让小模型「不敢 NONE」——给 reason 槽 + 真实理由示范

ollama JSON-schema 把输出锁进枚举后，小模型把任务退化成「挑最像的候选」，负例几乎
全错选；改成 `{reason, answer}` 结构化输出（先写一句判断依据再下结论）+ few-shot 的
assistant 侧带**真实短理由**，NONE 准确率 10%→86%。两个坑：reason 用占位符「…」会把
qwen3 全体带成 NONE 崩塌（示范必须真实）；few-shot 是 0.6~1.7B 级的硬要求（M0 已证）。

## 教训 4：候选约束式=LLM 兜底的安全底座

模型只在**确定性代码生成的候选枚举**里做选择（候选恒为 remainingText 真实子串），
答案必须精确命中枚举（代码复验）→ 模型无法制造文本里不存在的编辑对象，接管后走
零改动的 verified 执行链；任何失败回退旧行为。这与 Windows 跨轮纠错「模型出指令、
守卫代码层执行」同构——**LLM 参与破坏性操作的通用安全形态**：约束输出空间 +
确定性复验 + 失败回退。

## 教训 5（工程坑两则）

- **异步吞轮回退的重入循环**：对齐层吞掉本轮后，NONE 回退重入 `finishWithFinalText`
  会**再次**命中门控失败→再对齐→无限吞轮。修法：回退重入带 `allowTargetedAlign:false`
  （一次性语义）。任何「拦截→异步→回退到原入口」的结构都要防这类回环。
- swift build 新增源文件偶发不进构建计划（touch Package.swift 都不够），清
  `.build/out` 全量重建才可信；「cannot find in scope」先怀疑构建计划而非代码。

## 遗留

- poisoned 桶：C 层（isCommandFallback）不先落地，对齐层会在污染文本上「合理地」
  选错对象（评测 r3 实证）——**对齐层默认关的另一个理由，开启前先做 C 层**。
- llama.cpp 直连（不依赖 ollama 进程）与 Windows 同构待做；替换词侧漂移不在范围。

## 互链

- 排查与分层方法论：`targeted-edit-accuracy-layered-triage-2026-10-28.md`
- Windows 本地 LLM 构建坑：`windows-local-llm-refine-build-test-traps-2026-09-09.md`
- M0 spike（对照结论来源）：`../Plan/local-asr-accuracy-and-cross-turn-refinement.md`
