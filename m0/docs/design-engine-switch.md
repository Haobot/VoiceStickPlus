# 设计：引擎切换配置界面 + 本地 vs 云端闭环评测（M0 扩展）

> 需求：①评估当前本地识别效果；②配置界面在本地引擎/云端 ASR 间切换；
> ③用已录制音频对两类引擎做效率/准确度/综合效果的闭环自动化仿真验证；④评估报告与优化建议。
> 本文档为编码前方案确认（PCT 阶段一），实现遵循 TDD。

## 1. 架构（路线图 §6.2 EngineAdapter 的 M0 具象）

```text
web/index.html ──HTTP──> app.py (FastAPI, 127.0.0.1:8765)
                            │ 读写 config.toml / 触发评测 / 查询结果
sim_compare.py ─────────────┤
                            ▼
              src/m0_asr/providers/  ── 统一 AsrProvider 接口
              ├── local_sense_voice.py      SenseVoice int8 (sherpa-onnx)
              ├── local_seaco.py            SeACo 双层管线 (funasr, 注入 hotwords.txt)
              ├── cloud_tencent.py          腾讯云一句话识别 (HTTPS SDK)
              └── cloud_tencent_hotword     同上 + HotwordList 临时热词表
                            ▼
              compare_report.py → data/results/sim_compare_*.json
                                → docs/cloud_vs_local_report.md
```

> 落地注记：腾讯 SDK 3.1.170 的请求字段为 `Data`/`DataLen`（非二手文档的
> `AudioData`），详见 pitfalls.md 坑 12；`cloud_tencent_hotword` 变体利用
> 官方 `HotwordList` 临时热词表（"词|权重"），与本地 SeACo 读同一份
> hotwords.txt，保证热词维度的跨引擎公平对比。

## 2. Provider 接口（src/m0_asr/providers/base.py）

```python
class AsrProvider(ABC):
    name: str            # 注册键，如 "local_sense_voice"
    kind: str            # "local" | "cloud"
    def health_check(self) -> Health     # 模型权重/凭据是否可用，不发起识别
    def transcribe_file(self, wav) -> Outcome(
        text, elapsed_seconds, audio_seconds, error=None)

def get_provider(name, cfg) -> AsrProvider   # 注册表工厂
```

计时口径统一为**端到端**（调用开始→返回文本），云端含网络往返——这是用户体感的
真实口径；本地引擎另记纯推理时间供参考。延迟对比用同一口径，公平。

## 3. 凭据安全（红线）

云端凭据解析顺序：环境变量 `TENCENT_SECRET_ID/KEY/APPID` > `m0/config.toml`
（gitignored）> 复用产品配置 `%APPDATA%\VoiceStick\config.toml`（本机已有，
当前 `asr_provider = "tencent"`）。任何层级都不写入仓库；报告与日志输出脱敏。

无凭据时：云端引擎 health_check 返回 unavailable，评测对云端部分 SKIP 并在报告
如实标注（不 mock 云端结果——AGENTS.md 集成测试红线）。

## 4. 评测口径

- 数据集：已录制 29 条（9 基准 × 3 类 + 20 热词句），ground truth 人工标注
- 引擎：四引擎全跑（local_sense_voice / local_seaco / cloud_tencent /
  cloud_tencent_hotword），`--engines` 可参数化
- 指标：
  - 效率：延迟中位/均值/P95（端到端）、本地另报纯推理 RTF
  - 准确度：CER（总体+4 类分解）、热词未命中率（句级整词命中判定）
  - 综合：数据洞察段自动生成（RTF 达标判定/热词注入增益/混读 CER 排序）
  - 成本：按次计费以账单为准（编码前估的 ~¥0.0024/次 未在报告引用，避免臆造）
- 闭环：`python sim_compare.py` 一键全跑，报告自动生成，结果 JSON 留档可复算

## 5. 配置界面（web/index.html + app.py）

本地单页（原生 JS，无构建链）：
- 引擎卡片：kind/健康状态/当前激活，一键切换（写回 config.toml 的 `[engine].active`）
- 单句试听：选 wav → 用激活引擎识别，展示文本与耗时（切换前后的直接体感）
- 评测中心：勾选引擎 → 一键运行 → 进度 → 结果表（延迟/CER/热词/结论）
- API：`GET /api/engines`、`POST /api/engine/active`、`POST /api/transcribe`、
  `POST /api/eval/run`、`GET /api/eval/status`、`GET /api/eval/report`

## 6. 边界与不做

- 不做流式（腾讯流式协议复刻属 P1 桌面端职责）；一句话识别 API 与桌面端
  实时流式的延迟特性差异在报告口径中声明
- 不做并发压测（M0 串行逐句，关注单句质量与延迟分布）
- SeACo 本地热词引擎作为可选评测项纳入（其双层管线数据已在 hotword_report.md）
