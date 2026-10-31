# 本地全栈小模型评估 harness

评估「ASR + 文本精修全面本地小模型化」的可行性与最优组合。结论与数据见
`Doc/Test/local-fullstack-eval-2026-10-28.md`（评测报告）与
`Doc/Plan/local-first-voice-stack.md`（架构方案）。

## 组件

| 脚本 | 评什么 | 输入 |
|---|---|---|
| `run_asr_bench.py` | 本地 ASR：sherpa-onnx SenseVoice int8（Windows 在发同款）× whisper.cpp（tiny/base/small/large-v3-turbo-q5_0，zh/auto） | `scripts/e2e_test/corpus/` 31 条，3 轮 |
| `run_refine_bench.py` | 本地 LLM 精修：ollama 多模型 × prompt 形态（cloud 指令式 / cloud+热词 / fewshot / **editform 受限编辑** / det 纯确定性） | SenseVoice 第一轮输出 + 12 例合成填充词集 |

## 口径

- CER 复用云端基线同一实现（`scripts/e2e_test/asr_bench/metrics.py`，归一化
  含中文数字转阿拉伯），与 2026-08-01 云端基线（bench_results/）直接可比。
- 精修安全指标：`damage`（ΔCER>+0.01 的条数）、`clean_dmg`（原文全对被改错）。
- 延迟：SenseVoice=进程内稳态 decode；whisper=stderr timings 的 total-load。

## 环境

```bash
python3 -m venv .venv && .venv/bin/pip install -i https://pypi.tuna.tsinghua.edu.cn/simple sherpa-onnx numpy
brew install whisper.cpp   # ghcr.io 直连卡死时走代理 env
# 模型：models/sense-voice-int8-2024-07-17（ModelScope pengzhendong/...，
#   SHA-256 须与 scripts/pack_local_models.py 清单一致）+ ggml-*.bin（hf-mirror）
# ollama 常驻，模型按 --models 参数提前 pull
```

## 运行

```bash
.venv/bin/python run_asr_bench.py --engines sensevoice whisper-turbo-zh ...
.venv/bin/python run_refine_bench.py --models qwen2.5:1.5b-instruct ... \
    --variants cloud cloud+hw fewshot editform
.venv/bin/python run_refine_bench.py --models det --variants det --filler-variant det
```

结果落 `results/`（JSON+MD，入库）。模型权重/venv/wav 缓存已 gitignore。
