# M0 · 本地 ASR 引擎与热词验证（低语语音输入系统技术验证）

> 对应《产品设计路线图与系统架构 v1.3》§四 M0 与《M0 开发提示词_本地引擎与热词验证》
> （A：本地引擎跑通；B：热词注入验证）。
>
> **M0 通过线**：10 秒音频 < 1 秒出结果（RTF < 0.1）；热词命中错误率下降 ≥ 50%（至少一条路线）。

## 结论速览（详表见报告）

| 验收项 | 通过线 | 实测 | 状态 |
|---|---|---|---|
| A1 RTF 中位（SenseVoice int8，4 核 CPU） | < 0.1 | **0.016**（最大 0.019） | ✅ |
| A2 断网全流程 | 可复现 | 模型本地加载，识别零联网（TTS 生成阶段除外） | ✅ |
| A3 CER 基线（TTS 安静口径） | 参考 < 5% | **2.45%**（日常 1.04% / 混读 6.31% / 朗读 0%） | ✅ |
| A4 权重本地化 + 许可证 | models/ 本地留档 | models/MANIFEST.md（含 SHA256） | ✅ |
| B1 热词测试集 | 10 词 × 2 句人工标注 | `data/texts/testset.json` | ✅ |
| B2/B3/B4 双路线对比 | 至少一条 ≥ 50% 降幅 | 见 `hotword/hotword_report.md` | 见报告 |
| B5 选型建议 | 数据 + 许可证支撑 | 见 `hotword/hotword_report.md` | ✅ |

## 目录结构

```text
m0/
├── transcribe.py            # 单文件/麦克风识别（提示词 A 任务 3）
├── benchmark.py             # 基准测试：3 类 × 3 条 × 3 次中位（任务 4）
├── eval.py                  # jiWER CER 评测 + 报告生成（任务 5）
├── hotword/
│   ├── hotword_seaco.py     # 路线一：FunASR SeACo 热词双跑（任务 3）
│   ├── hotword_qwen3.py     # 路线二：Qwen3-ASR context 偏置双跑（任务 4）
│   └── make_hotword_report.py  # 汇总报告（任务 5）
├── src/m0_asr/              # 核心包（metrics / engine / audio_utils / model_registry）
├── tests/                   # pytest 单测（42 项，TDD 红-绿-重构）
├── scripts/
│   ├── download_models.py   # 模型下载（ModelScope → hf-mirror → GitHub 三级回退）
│   └── gen_test_audio.py    # 测试音频生成（edge-tts，仅此步联网）
├── data/
│   ├── texts/testset.json   # 语料与 ground truth（人工标注）
│   ├── wavs/                # 生成的测试音频（gitignored，可再生成）
│   └── results/             # 识别结果缓存（验收证据，进 git）
├── models/                  # 模型权重（gitignored，见 MANIFEST.md）
└── docs/
    ├── benchmark_report.md  # RTF + CER 基线报告（提示词 A 交付物）
    └── pitfalls.md          # 坑记录（验收材料的一部分）
```

## 一键复现（Windows 11 + Python 3.10+）

```bash
cd m0
python -m venv .venv                     # 或指定 Python 3.12
.venv/Scripts/python.exe -m pip install -r requirements.txt -i https://pypi.tuna.tsinghua.edu.cn/simple

# 1) 下载模型（约 1.4GB；国内三级源回退，支持断点续传）
.venv/Scripts/python.exe scripts/download_models.py --all

# 2) 生成测试音频（29 条；需联网调用 edge-tts，识别运行时不需要网络）
.venv/Scripts/python.exe scripts/gen_test_audio.py

# 3) 提示词 A：基准测试 + CER 评测 → docs/benchmark_report.md
.venv/Scripts/python.exe benchmark.py
.venv/Scripts/python.exe eval.py

# 4) 单文件识别 / 麦克风实时验证
.venv/Scripts/python.exe transcribe.py data/wavs/daily_01.wav
.venv/Scripts/python.exe transcribe.py --mic 10

# 5) 提示词 B：热词双路线 → hotword/hotword_report.md
.venv/Scripts/python.exe hotword/hotword_seaco.py
.venv/Scripts/python.exe hotword/hotword_qwen3.py
.venv/Scripts/python.exe hotword/make_hotword_report.py

# 6) 单元测试
.venv/Scripts/python.exe -m pytest tests/ -q
```

### 模型手动下载后备方案

若自动下载全部失败，按 `models/MANIFEST.md` 的清单手动放置：

| 模型 | 主源 | 备选 |
|---|---|---|
| SenseVoice int8 | `github.com/k2-fsa/sherpa-onnx/releases` asr-models tag | `hf-mirror.com/csukuangfj/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17` |
| SeACo（FunASR） | `modelscope.cn/models/iic/speech_seaco_paraformer_large_asr_nat-zh-cn-16k-common-vocab8404-pytorch` | — |
| Qwen3-ASR 0.6B int8 | GitHub asr-models | `hf-mirror.com/pantinor/sherpa-onnx-qwen3-asr-0.6b-int8` |

放到 `models/` 下对应目录（目录名见 `src/m0_asr/model_registry.py`）后重跑
`download_models.py` 会校验完整性并跳过下载。

## 口径与已知边界（如实记录）

- **TTS 音频口径**：edge-tts 合成（标准发音、无噪声）≈"安静环境朗读"。真实嘈杂/口音/
  **耳语**场景不在覆盖范围——耳语基线（M0 生死判据）需真人耳语录音另行测试。
- **CER 从严口径**：汉字数字 vs 阿拉伯数字（"三点"/"3点"）未做等价转换，计入错误。
- **计时口径**：SenseVoice/Qwen3 计 `decode_stream`（不含加载与 IO）；funasr 计端到端
  `generate`。各路线基线/偏置双跑内部同口径，降幅对比公平；跨路线绝对延迟见报告说明。
- **热词命中判定**：归一化（全角/标点/大小写）后整词子串匹配，不做读音变体匹配。
- ground truth 全部人工标注，未用任何 ASR 输出互校。

## 离线性说明

识别链路（transcribe / benchmark / eval / hotword_*）只读取本地权重与本地音频，
无任何网络请求。唯一联网环节是测试音频生成（edge-tts）与模型下载脚本。
