# targeted 目标词本地对齐——闭环评测 harness

对 `desktop/macos/Sources/VoiceStickCore/VoiceTargetedAlign.swift`（候选生成 + prompt 构造）
与本地 ollama 小模型的**闭环评测**：数据集 → 真实 Core 逻辑生成候选与请求体 →
`/api/chat`（think:false + JSON-schema enum 硬约束）→ 同口径解析 → 自动打分出报告。
用于模型选型与 prompt/预筛参数迭代，评测结论见
`Doc/Test/local-llm-target-alignment-eval-2026-10-28.md`。

## 依赖

- 本机 ollama（默认 `http://127.0.0.1:11434`）并拉取候选模型
  （当前推荐 `qwen2.5:1.5b-instruct`）。
- swiftc（编译 bench CLI，链接真实 Core 源文件；无需完整 app 构建）。

## 用法

```bash
cd scripts/local_align_bench
# 1. 编译 CLI（输入 JSONL → 输出候选 + 与 App 同构的请求体）
VOICESTICK_ARCHS=arm64 swiftc -O -o /tmp/align_cli cli_main.swift \
  ../../desktop/macos/Sources/VoiceStickCore/VoiceTargetedAlign.swift \
  ../../desktop/macos/Sources/VoiceStickCore/VoiceTargetedEdit.swift \
  ../../desktop/macos/Sources/VoiceStickCore/VoiceEditCommand.swift \
  ../../desktop/macos/Sources/VoiceStickCore/PinyinGuard.swift \
  ../../desktop/macos/Sources/VoiceStickCore/PinyinData.swift \
  && cp /tmp/align_cli cli/align_cli
# 2. 跑评测（主集 + 留出集）
python3 run_bench.py --models qwen2.5:1.5b-instruct gemma3:1b ...
```

- `dataset.json`：主集（真实失败样本 + 同音/近音合成 + 负例 + gate/poisoned 分桶）。
- `dataset_holdout.json`：留出集（全新词汇，防 prompt 过拟合）。
- 输出 `out/results.json` + `out/report.md`（含逐例归因）。
- 注意：CLI 源 `cli_main.swift` 与 `cli/` 由首次使用时准备（对齐到当前 Core 版本），
  `cli/align_cli` 为本机产物不入库。

## 指标口径

- `align`：正例（期望为 remaining 子串）中模型答对比例（分母已排除 gate/poisoned 桶）。
- `none`：负例（期望 NONE）答 NONE 比例。
- `wrong`：答了候选但不是期望词（潜在错编辑；其中 gate 桶 id 不进生产路径，不计风险）。
- 延迟为完整 HTTP 往返（含首呼模型加载的 p95）。
