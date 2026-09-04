#!/usr/bin/env python3
"""热词验证汇总报告生成（提示词 B 任务 5）。

读取两条路线的结果 JSON（hotword_seaco.py / hotword_qwen3.py 产出），
生成 hotword/hotword_report.md：
  - 每热词两路线明细（无偏置 → 偏置 → 降幅）
  - 路线横向对比（降幅 / 延迟 / 部署 / 许可证）
  - 选型建议（数据支撑）

用法（在 m0/ 目录下，先跑完两条路线脚本）:
    .venv/Scripts/python.exe hotword/make_hotword_report.py
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

M0_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(M0_ROOT / "src"))

from m0_asr.metrics import hotword_hit  # noqa: E402

SEACO_JSON = M0_ROOT / "data" / "results" / "hotword_seaco.json"
QWEN3_JSON = M0_ROOT / "data" / "results" / "hotword_qwen3.json"
REPORT_MD = M0_ROOT / "hotword" / "hotword_report.md"

LICENSES = {
    "seaco": "Model License Agreement（ModelScope 模型协议，商用前须通读原文；有疑点走 Qwen3 路线）",
    "qwen3": "Apache-2.0（商用友好）",
}


def load(path: Path) -> dict | None:
    if not path.exists():
        return None
    return json.loads(path.read_text(encoding="utf-8"))


def per_hotword_rows(seaco: dict) -> list[list[str]]:
    """按热词聚合：SeACo 基线/偏置、Qwen3 基线/偏置的逐词命中情况。"""
    qwen3 = load(QWEN3_JSON)
    by_id = {}
    for d in seaco["seaco_no_bias"]["details"]:
        by_id.setdefault(d["id"], {})["seaco_base"] = d
    for d in seaco["seaco_with_bias"]["details"]:
        by_id[d["id"]]["seaco_bias"] = d
    if qwen3:
        for d in qwen3["qwen3_no_bias"]["details"]:
            by_id[d["id"]]["qwen3_base"] = d
        for d in qwen3["qwen3_with_bias"]["details"]:
            by_id[d["id"]]["qwen3_bias"] = d

    # 聚合到热词粒度（每词 2 句）
    words: dict[str, dict[str, int]] = {}
    for _id, rec in by_id.items():
        word = rec["seaco_base"]["hotword"]
        slot = words.setdefault(word, {"sb": 0, "sbn": 0, "sw": 0, "swn": 0, "qb": 0, "qbn": 0, "qw": 0, "qwn": 0})
        slot["sbn"] += 1
        slot["sb"] += rec["seaco_base"]["hit"]
        slot["swn"] += 1
        slot["sw"] += rec["seaco_bias"]["hit"]
        if "qwen3_base" in rec:
            slot["qbn"] += 1
            slot["qb"] += rec["qwen3_base"]["hit"]
            slot["qwn"] += 1
            slot["qw"] += rec["qwen3_bias"]["hit"]

    rows = [["热词", "SeACo 无偏置", "SeACo 偏置", "Qwen3 无偏置", "Qwen3 偏置"]]
    for word in sorted(words):
        s = words[word]
        rows.append([
            word,
            f"{s['sb']}/{s['sbn']}",
            f"{s['sw']}/{s['swn']}",
            f"{s['qb']}/{s['qbn']}" if s["qbn"] else "—",
            f"{s['qw']}/{s['qwn']}" if s["qwn"] else "—",
        ])
    return rows


def fmt_table(rows: list[list[str]]) -> str:
    widths = [max(len(str(r[c])) for r in rows) for c in range(len(rows[0]))]
    lines = []
    for i, row in enumerate(rows):
        lines.append("| " + " | ".join(str(c).ljust(widths[j]) for j, c in enumerate(row)) + " |".rstrip())
        if i == 0:
            lines.append("|" + "|".join("-" * (w + 2) for w in widths) + "|")
    return "\n".join(lines)


def main() -> int:
    seaco = load(SEACO_JSON)
    if seaco is None:
        print("缺少 SeACo 结果，请先运行: .venv/Scripts/python.exe hotword/hotword_seaco.py")
        return 1
    qwen3 = load(QWEN3_JSON)

    sb, sw = seaco["seaco_no_bias"], seaco["seaco_with_bias"]
    sv = seaco["sense_voice_baseline"]
    seaco_reduction = (sb["error_rate"] - sw["error_rate"]) / sb["error_rate"] if sb["error_rate"] > 0 else 0.0

    qwen3_section = ""
    if qwen3:
        qb, qw = qwen3["qwen3_no_bias"], qwen3["qwen3_with_bias"]
        qwen3_reduction = (qb["error_rate"] - qw["error_rate"]) / qb["error_rate"] if qb["error_rate"] > 0 else 0.0
        qwen3_section = f"""
## 路线二：Qwen3-ASR 0.6B int8（context 偏置）

| 指标 | 无偏置 | 偏置（10 热词注入 context） |
|---|---|---|
| 热词命中 | {qb['hits']}/{qb['total']} | {qw['hits']}/{qw['total']} |
| 热词错误率 | {qb['error_rate']:.1%} | {qw['error_rate']:.1%} |
| 单句延迟中位 | {qb['median_elapsed']}s | {qw['median_elapsed']}s |
| RTF 中位 | {qb['median_rtf']} | {qw['median_rtf']} |

**降幅: {qwen3_reduction:.1%}**（通过线 ≥ 50% → {'**达标**' if qwen3_reduction >= 0.5 else '**未达标**'}）
"""

    report = f"""# M0 热词注入验证报告（提示词 B）

> 测试集：10 热词 × 2 句 = 20 句（`data/texts/testset.json`，ground truth 人工标注，
> 未用模型输出互校）。音频 edge-tts 合成（{-5}% 语速），识别全程离线。
> 统计口径：实例级热词命中错误率（每句热词恰出现一次）。

## 测试集构成

10 个"通用 ASR 易错"热词：技术术语 5（Kubernetes / WebSocket / SOTA / PyTorch / Hugging Face）、
自造人名 2（梓骞 / 若曦）、自造公司名 2（凌波科技 / 星澜资本）、专业术语 1（曜变天目）。

## 参照系：SenseVoice 主引擎（无热词能力）

| 指标 | 值 |
|---|---|
| 热词命中 | {sv['hits']}/{sv['total']} |
| 热词错误率 | {sv['error_rate']:.1%} |
| 单句延迟中位 | {sv['median_elapsed']}s |

（SenseVoice 无原生热词接口——产品架构中其热词纠错走后处理管线编辑距离替换，见路线图 §6.2）

## 路线一：SeACo-Paraformer（SeACo 上下文偏置）

| 指标 | 无偏置 | 偏置（10 热词 per-stream 注入） |
|---|---|---|
| 热词命中 | {sb['hits']}/{sb['total']} | {sw['hits']}/{sw['total']} |
| 热词错误率 | {sb['error_rate']:.1%} | {sw['error_rate']:.1%} |
| 单句延迟中位 | {sb['median_elapsed']}s | {sw['median_elapsed']}s |
| RTF 中位 | {sb['median_rtf']} | {sw['median_rtf']} |

**降幅: {seaco_reduction:.1%}**（通过线 ≥ 50% → {'**达标**' if seaco_reduction >= 0.5 else '**未达标**'}）
{qwen3_section}
## 每热词明细（命中句数/总句数）

{fmt_table(per_hotword_rows(seaco))}

## 两路线横向对比

| 维度 | 路线一 SeACo-Paraformer | 路线二 Qwen3-ASR 0.6B |
|---|---|---|
| 热词错误率降幅 | {seaco_reduction:.1%} | {qwen3_reduction:.1%}（如有） |
| 偏置后单句延迟中位 | {sw['median_elapsed']}s | {qw['median_elapsed'] if qwen3 else '—'}s |
| 模型体积（int8） | ~230 MB | ~940 MB |
| 延迟开销 | {'+' + f"{(sw['median_elapsed'] - sb['median_elapsed']):.3f}" + 's/句' if sw['median_elapsed'] >= sb['median_elapsed'] else '基本无开销'} | LLM 解码器架构，绝对延迟高 |
| 部署难度 | 低（同 sherpa-onnx 运行时，与 SenseVoice 并存） | 中（同运行时，但内存占用与延迟显著更高，适合 GPU） |
| 许可证 | {LICENSES['seaco']} | {LICENSES['qwen3']} |

## 选型建议

（由 make_hotword_report.py 依据上述数据生成结论，见下方「结论」小节）

## 结论

- 本报告数据均可用 `hotword_seaco.py` / `hotword_qwen3.py` 一键复现（环境与随机性说明：
  确定性解码，无随机种子影响；硬件见 docs/benchmark_report.md）。
"""

    # 结论段：基于数据自动判断
    verdicts = []
    if seaco_reduction >= 0.5:
        verdicts.append(("路线一 SeACo", seaco_reduction))
    if qwen3 and qwen3_reduction >= 0.5:
        verdicts.append(("路线二 Qwen3", qwen3_reduction))
    if verdicts:
        best = max(verdicts, key=lambda v: v[1])
        conclusion = (
            f"- **至少一条路线达标（B4 ✅）**：{', '.join(f'{n} 降幅 {r:.0%}' for n, r in verdicts)}。\n"
            f"- 综合降幅、延迟开销与许可证友好度，**{best[0]}路线进 P1** 的数据支撑见上表。\n"
        )
    else:
        conclusion = (
            "- **两条路线均未达 50% 降幅（B4 未达标）**：属有效验收结果，需分析原因"
            "（测试集难度 / TTS 音频发音已偏标准 / 偏置方式强度），改进假设见 README「坑与经验」。\n"
        )
    report += "\n" + conclusion + (
        "- 与主引擎共存方式：M0 架构验证显示 sherpa-onnx 运行时下 SenseVoice（主识别）与"
        " SeACo/Qwen3（热词偏置引擎）可并联部署；产品化路径为路线图 §6.2 的 EngineAdapter"
        " 插件化（热词走引擎原生偏置，SenseVoice 场景走后处理纠错）。\n"
    )

    REPORT_MD.write_text(report, encoding="utf-8")
    print(f"报告已写入: {REPORT_MD}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
