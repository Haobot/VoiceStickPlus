"""闭环对比评测的指标聚合与报告渲染。

输入是 sim_compare 管线产出的逐句行（dict），口径约定：
- 失败句（error 非空）不计入 CER 与热词统计，单独计 failures；
- 热词句判定 = 标注了热词（hotword 非空）或 hit 显式为 False（未命中）；
  热词未命中率 = hit 不为 True 的热词句占比。
"""
from __future__ import annotations

import math
import statistics
from dataclasses import dataclass, field

from m0_asr.metrics import cer


@dataclass(frozen=True)
class EngineMetrics:
    """单引擎在全测试集上的聚合指标。"""

    engine_name: str
    total: int  # 成功句数
    failures: int  # 失败句数
    cer_overall: float  # 逐句 CER 平均（失败句除外）
    hotword_total: int
    hotword_misses: int
    error_rate: float  # 热词未命中率 = misses / hotword_total
    latency: dict = field(default_factory=dict)  # median/mean/p95（秒，成功句）


def latency_stats(values: list[float]) -> dict:
    """延迟分布：中位 / 均值 / P95（秒）。空列表全 0.0。"""
    if not values:
        return {"median": 0.0, "mean": 0.0, "p95": 0.0}
    ordered = sorted(values)
    n = len(ordered)
    p95_index = min(math.ceil(0.95 * n) - 1, n - 1)
    return {
        "median": statistics.median(ordered),
        "mean": statistics.fmean(ordered),
        "p95": ordered[p95_index],
    }


def _is_hotword_row(row: dict) -> bool:
    return bool(row.get("hotword")) or row.get("hit") is False


def aggregate(engine_name: str, rows: list[dict]) -> EngineMetrics:
    """把逐句结果行聚合为 EngineMetrics。"""
    failures = sum(1 for r in rows if r.get("error"))
    ok_rows = [r for r in rows if not r.get("error")]
    cers = [cer(r["text_gt"], r["text_asr"]) for r in ok_rows]
    cer_overall = statistics.fmean(cers) if cers else 0.0
    hotword_rows = [r for r in ok_rows if _is_hotword_row(r)]
    hotword_misses = sum(1 for r in hotword_rows if r.get("hit") is not True)
    hotword_total = len(hotword_rows)
    error_rate = (hotword_misses / hotword_total) if hotword_total else 0.0
    lat = latency_stats([r["elapsed_seconds"] for r in ok_rows])
    return EngineMetrics(engine_name=engine_name, total=len(ok_rows),
                         failures=failures, cer_overall=cer_overall,
                         hotword_total=hotword_total,
                         hotword_misses=hotword_misses, error_rate=error_rate,
                         latency=lat)


def render_report(metrics: dict[str, EngineMetrics], dataset_note: str = "") -> str:
    """渲染 Markdown 对比报告（含结论与建议段）。"""
    lines = ["# 本地 vs 云端 ASR 对比评测报告", ""]
    if dataset_note:
        lines += [f"- 数据集口径: {dataset_note}", ""]
    if not metrics:
        lines += ["暂无可评测结果（无引擎就绪或测试集为空）。", ""]
        return "\n".join(lines)

    lines += [
        "| 引擎 | 成功/失败 | CER | 热词句 | 热词未命中 | 未命中率 | 中位延迟 | P95 |",
        "|---|---|---|---|---|---|---|---|",
    ]
    for name, m in metrics.items():
        lines.append(
            f"| {name} | {m.total}/{m.failures} | {m.cer_overall:.2%} "
            f"| {m.hotword_total} | {m.hotword_misses} | {m.error_rate:.1%} "
            f"| {m.latency.get('median', 0):.3f}s | {m.latency.get('p95', 0):.3f}s |"
        )
    lines += [""]

    lines += ["## 结论与建议", ""]
    names = list(metrics)
    best_cer = min(names, key=lambda n: metrics[n].cer_overall)
    best_lat = min(names, key=lambda n: metrics[n].latency.get("median", math.inf))
    lines += [
        f"- 准确度最优: **{best_cer}**（CER {metrics[best_cer].cer_overall:.2%}）",
        f"- 延迟最优: **{best_lat}**（中位 {metrics[best_lat].latency.get('median', 0):.3f}s）",
        "- 建议: 本地引擎满足 RTF<0.1 且 CER 达标时优先本地（离线、零成本、隐私）；"
        "云端作为弱网/复杂场景的 fallback，由配置界面切换。",
        "",
    ]
    return "\n".join(lines)
