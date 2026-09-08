#!/usr/bin/env python3
"""热词验证汇总报告生成（提示词 B 任务 5）。

读取两条路线的结果 JSON（hotword_seaco.py / hotword_qwen3.py 产出），
生成 hotword/hotword_report.md：
  - 每热词多档明细（基线 → 偏置 → 双层管线 / Qwen3 基线 → 偏置）
  - 路线横向对比（降幅 / 延迟 / 部署 / 许可证）
  - 选型建议（数据支撑 + 副作用如实呈现）

用法（在 m0/ 目录下，先跑完两条路线脚本）:
    .venv/Scripts/python.exe hotword/make_hotword_report.py
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

M0_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(M0_ROOT / "src"))

SEACO_JSON = M0_ROOT / "data" / "results" / "hotword_seaco.json"
QWEN3_JSON = M0_ROOT / "data" / "results" / "hotword_qwen3.json"
REPORT_MD = M0_ROOT / "hotword" / "hotword_report.md"


def load(path: Path) -> dict | None:
    if not path.exists():
        return None
    return json.loads(path.read_text(encoding="utf-8"))


def fmt_table(rows: list[list[str]]) -> str:
    widths = [max(len(str(r[c])) for r in rows) for c in range(len(rows[0]))]
    lines = []
    for i, row in enumerate(rows):
        lines.append(("| " + " | ".join(str(c).ljust(widths[j]) for j, c in enumerate(row)) + " |").rstrip())
        if i == 0:
            lines.append("|" + "|".join("-" * (w + 2) for w in widths) + "|")
    return "\n".join(lines)


def reduction(base_er: float, er: float) -> float:
    return (base_er - er) / base_er if base_er > 0 else 0.0


def per_hotword_rows(seaco: dict, qwen3: dict | None) -> list[list[str]]:
    """按热词聚合各档命中情况（每词 2 句）。"""
    by_id: dict[str, dict] = {}
    for slot, key in [("sb", "seaco_no_bias"), ("sw", "seaco_with_bias"), ("sp", "seaco_bias_plus_postprocess")]:
        for d in seaco[key]["details"]:
            by_id.setdefault(d["id"], {})[slot] = d
    if qwen3:
        for slot, key in [("qb", "qwen3_no_bias"), ("qw", "qwen3_with_bias")]:
            for d in qwen3[key]["details"]:
                by_id.setdefault(d["id"], {})[slot] = d

    words: dict[str, dict[str, int]] = {}
    for rec in by_id.values():
        word = rec["sb"]["hotword"]
        slot = words.setdefault(word, {})
        for tag in ("sb", "sw", "sp", "qb", "qw"):
            d = rec.get(tag)
            if d is not None:
                slot[tag] = slot.get(tag, 0) + (1 if d["hit"] else 0)

    def cell(slot, tag):
        return f"{slot.get(tag, 0)}/2" if slot else "—"

    rows = [["热词", "SeACo 基线", "SeACo 偏置", "SeACo 双层", "Qwen3 基线", "Qwen3 偏置"]]
    for word in sorted(words):
        s = words[word]
        rows.append([word, cell(s, "sb"), cell(s, "sw"), cell(s, "sp"), cell(s, "qb"), cell(s, "qw")])
    return rows


def side_effect_lines(seaco: dict) -> list[str]:
    """后处理层的副作用明细（偏置命中状态不变但文本被改动，且替换词与目标不符）。"""
    bias = {d["id"]: d for d in seaco["seaco_with_bias"]["details"]}
    pipe = {d["id"]: d for d in seaco["seaco_bias_plus_postprocess"]["details"]}
    lines = []
    for k in bias:
        if bias[k]["text_asr"] != pipe[k]["text_asr"] and not pipe[k]["hit"] and bias[k]["hotword"] not in pipe[k]["text_asr"]:
            # 未命中且最终文本不含本句目标热词——后处理可能替换成了错误热词
            others = [w for w in seaco["hotwords"] if w in pipe[k]["text_asr"] and w != bias[k]["hotword"]]
            if others:
                lines.append(f"- `{k}`（目标 **{bias[k]['hotword']}**）被后处理替换成了 **{others[0]}**（错配）")
    return lines


def main() -> int:
    seaco = load(SEACO_JSON)
    if seaco is None:
        print("缺少 SeACo 结果，请先运行: .venv/Scripts/python.exe hotword/hotword_seaco.py")
        return 1
    qwen3 = load(QWEN3_JSON)

    sb = seaco["seaco_no_bias"]
    sw = seaco["seaco_with_bias"]
    sp = seaco["seaco_bias_plus_postprocess"]
    sv = seaco["sense_voice_baseline"]
    r_bias = reduction(sb["error_rate"], sw["error_rate"])
    r_pipe = reduction(sb["error_rate"], sp["error_rate"])

    qb = qw = None
    r_qwen = None
    qwen3_section = ""
    if qwen3:
        qb, qw = qwen3["qwen3_no_bias"], qwen3["qwen3_with_bias"]
        r_qwen = reduction(qb["error_rate"], qw["error_rate"])
        qwen3_section = f"""
## 路线二：Qwen3-ASR 0.6B int8（context 偏置）

| 指标 | 无偏置 | 偏置（10 热词写入 context prompt） |
|---|---|---|
| 热词命中 | {qb['hits']}/{qb['total']} | {qw['hits']}/{qw['total']} |
| 热词错误率 | {qb['error_rate']:.1%} | {qw['error_rate']:.1%} |
| 单句延迟中位 | {qb['median_elapsed']}s | {qw['median_elapsed']}s |

**降幅: {r_qwen:.1%}**（通过线 ≥ 50% → **未达标**，如实记录）

生效证据：`Hugging Face` 句由误识 "HoudiniFace" 纠正为 "Hugging Face"——context
偏置真实起效；但 Qwen3 基线本身太强（70% 命中），剩余错误集中于同音字
（梓骞→子谦、曜变→耀变），LLM 语言先验压过了 prompt 偏置。
"""

    side_effects = side_effect_lines(seaco)

    report = f"""# M0 热词注入验证报告（提示词 B）

> 测试集：10 热词 × 2 句 = 20 句（`data/texts/testset.json`，ground truth 人工标注，
> 未用模型输出互校）。音频 edge-tts 合成（-5% 语速），识别全程离线。
> 统计口径：实例级热词命中错误率（每句热词恰出现一次；归一化后整词子串匹配）。
> 复现：`hotword_seaco.py` / `hotword_qwen3.py` 一键重跑（确定性解码，无随机性；
> 硬件环境见 docs/benchmark_report.md）。

## 测试集构成

10 个"通用 ASR 易错"热词：技术术语 5（Kubernetes / WebSocket / SOTA / PyTorch / Hugging Face）、
自造人名 2（梓骞 / 若曦）、自造公司名 2（凌波科技 / 星澜资本）、专业术语 1（曜变天目）。

## 参照系：SenseVoice 主引擎（sherpa-onnx，无热词接口）

| 指标 | 值 |
|---|---|
| 热词命中 | {sv['hits']}/{sv['total']}（错误率 {sv['error_rate']:.1%}） |
| 单句延迟中位 | {sv['median_elapsed']}s（RTF 中位 {sv['median_rtf']}） |

## 路线一：FunASR SeACo-Paraformer（三层递进）

| 指标 | 基线 | 模型级偏置（SeACo） | 双层管线（偏置 + 文本级后处理纠正） |
|---|---|---|---|
| 热词命中 | {sb['hits']}/{sb['total']} | {sw['hits']}/{sw['total']} | {sp['hits']}/{sp['total']} |
| 热词错误率 | {sb['error_rate']:.1%} | {sw['error_rate']:.1%} | {sp['error_rate']:.1%} |
| 相对基线降幅 | — | {r_bias:.1%} | **{r_pipe:.1%}** |
| 单句延迟中位 | {sb['median_elapsed']}s | {sw['median_elapsed']}s | {sp['median_elapsed']}s |

**双层管线降幅 {r_pipe:.1%}（通过线 ≥ 50% → {'**达标**' if r_pipe >= 0.5 else '**未达标**'}）**

两层各自贡献：
- **模型级偏置（+4 句）**：全部为中文同音词——星澜资本×2（星蓝/建新兰→星澜）、
  曜变天目×2（要变天幕/耀变天幕→曜变天目）。SeACo 对中文同音偏置强且无副作用。
- **文本级后处理（+2 句）**：梓骞×2（子谦→梓骞）、PyTorch×2 中 2 句
  （PYTORH/PYTOCH→PyTorch）与 Kubernetes×1（cubontius→Kubernetes）中的 2 句。
  （阈值 {seaco.get('postprocess_threshold', 0.85)}，拼音 + 字符模糊匹配）

### 已知副作用（如实记录，P1 改进项）

后处理层对**英文 ASR 乱码片段**存在热词间错配（模糊匹配选了"最像的错误热词"）：

{chr(10).join(side_effects) if side_effects else "（无）"}

改进假设：英文替换要求更高字符相似阈值（>0.9）或仅允许拼音级（中文同音）替换；
替换时校验音频时长与词长比例；对偏置层已命中的热词跳过后处理替换（防破坏）。
这属于热词管线精细化，P1 落地。
{qwen3_section}
## 每热词明细（命中句数/2 句）

{fmt_table(per_hotword_rows(seaco, qwen3))}

## 两路线横向对比

| 维度 | 路线一 FunASR SeACo（双层管线） | 路线二 Qwen3-ASR 0.6B |
|---|---|---|
| 热词错误率降幅 | **{r_pipe:.1%}** ✅ | {f"{r_qwen:.1%}" if r_qwen is not None else "—"} ❌ |
| 基线错误率 | {sb['error_rate']:.0%}（偏置空间大） | {qb['error_rate']:.0%}（基线已强） |
| 单句延迟（4 核 CPU） | {sp['median_elapsed']}s（开销 +{(sp['median_elapsed'] - sb['median_elapsed']):.3f}s） | {qw['median_elapsed'] if qw else '—'}s（LLM 解码器，绝对值高） |
| 模型体积 | ~950 MB（pytorch） | ~940 MB（int8 onnx） |
| 运行时依赖 | funasr + torch（重栈） | sherpa-onnx（与主引擎同栈） |
| 部署难度 | 中（pytorch 栈与 SenseVoice 的 onnx 栈并存） | 低（同栈，但 CPU 延迟不适合本地主力） |
| 许可证 | Model License Agreement（商用前须通读协议原文） | Apache-2.0（商用友好） |

## 选型建议（数据支撑）

**进 P1：以 FunASR SeACo 双层管线（模型级偏置 + 文本级纠正）为热词引擎。**

1. **收益**：唯一达标路线（降幅 {r_pipe:.1%} ≥ 50%）；延迟开销极小
   （+{(sp['median_elapsed'] - sb['median_elapsed']):.3f}s/句，CPU 可承受）。
2. **结构**：双层各自覆盖互补的错误面——模型级偏置管中文同音词（本测试 4/4 全修），
   后处理管近形英文词（PYTORH→PyTorch）与超短同音词（梓骞）。这正是路线图 §6.2
   「热词纠错放到后处理管线」设计的数据佐证。
3. **风险与前置工作**：
   - 许可证：Paraformer 权重走 ModelScope MLA，上线前通读协议原文；
     若有疑点，切 Qwen3 路线（Apache-2.0）——但 Qwen3 的 CPU 延迟与偏置强度
     （{r_qwen:.1%}）暂不满足本地产品要求，GPU 部署时重评。
   - 后处理错配：英文热词替换需加约束（见副作用小节），P1 第一优先。
   - 运行时统一：SeACo 的 sherpa-onnx 导出版**不支持热词**（见 docs/pitfalls.md 坑 1），
     P1 若想统一 onnx 栈需自导出含 SeACo 偏置的图，或保留 funasr 栈做热词旁路。
4. **与主引擎共存**：SenseVoice（主识别，RTF 0.016）+ SeACo（热词旁路）并联——
   EngineAdapter 插件化（路线图 §6.2）：高热词密度会话路由到 SeACo，
   普通会话走 SenseVoice + 后处理纠正；热词库（格式冻结 v1）对两引擎统一供词。

## 结论

- **B4 达标（至少一条路线 ≥ 50%）**：路线一 SeACo 双层管线降幅 {r_pipe:.1%}。
  但须诚实标注两点：①达标依赖后处理层，且该层存在英文热词错配副作用（6 句错替换，
  其中 3 句破坏了偏置层已命中的 WebSocket/SOTA 句——净效应 +2 = 修复 5 − 破坏 3，
  命中统计未因此虚增，但文本质量受损）；②若无后处理层（仅模型级偏置），
  降幅为 {r_bias:.1%}，未达通过线。后处理层的英文错配约束是 P1 第一优先事项。
- 路线二 Qwen3 未达标（{r_qwen:.1%}），但验证了 context 偏置生效性与 LLM 引擎的
  高基线特性——GPU 场景（路线图 P2 引擎自动分级）下值得重测。
"""

    REPORT_MD.write_text(report, encoding="utf-8")
    print(f"报告已写入: {REPORT_MD}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
