"""JSON + Markdown 报告生成。"""
from __future__ import annotations

import json
from datetime import datetime, timezone
from pathlib import Path

from .metrics import per_clip_detail, summarize
from .result import ClipResult


def build_json_report(results: list[ClipResult], *, corpus_size: int,
                      rounds: int) -> dict:
    providers = sorted({r.provider for r in results})
    return {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "corpus_size": corpus_size,
        "rounds": rounds,
        "providers": {
            p: summarize([r for r in results if r.provider == p]) for p in providers
        },
        "clips": per_clip_detail(results),
    }


def _fmt(value, digits=2, suffix="") -> str:
    if value is None:
        return "—"
    if isinstance(value, float):
        return f"{value:.{digits}f}{suffix}"
    return f"{value}{suffix}"


def build_markdown(report: dict) -> str:
    lines = [
        "# ASR 离线评测基线报告",
        "",
        f"- 生成时间：{report['generated_at']}",
        f"- 语料规模：{report['corpus_size']} 条，每平台 {report['rounds']} 轮全量回放（压力/稳定性测试）",
        f"- 方法：本地 Ogg Opus 语料按真实时长实时节奏回放进各家 ASR WebSocket，"
        f"协议与桌面端一致；不带热词/自学习表（客观基线）；CER 按归一化文本"
        f"（去标点/空白、大小写与全半角归一、中文数字转阿拉伯）计算。",
        "",
        "## 总览对比",
        "",
        "| 指标 | " + " | ".join(report["providers"].keys()) + " |",
        "|---|" + "---|" * len(report["providers"]),
    ]

    def row(label, fn):
        cells = [fn(report["providers"][p]) for p in report["providers"]]
        lines.append(f"| {label} | " + " | ".join(cells) + " |")

    row("运行数（成功/总数）", lambda s: f"{s['success']}/{s['runs']}")
    row("失败率", lambda s: _fmt(s["failure_rate"], 4))
    row("CER 均值", lambda s: _fmt(s["cer_mean"], 4))
    row("CER 中位数", lambda s: _fmt(s["cer_median"], 4))
    row("CER 最差", lambda s: _fmt(s["cer_max"], 4))
    row("首 partial 延迟 p50/p95 (ms)",
        lambda s: f"{_fmt(s['first_partial_latency_ms']['p50'], 0)} / "
                  f"{_fmt(s['first_partial_latency_ms']['p95'], 0)}")
    row("尾延迟 p50/p95 (ms)",
        lambda s: f"{_fmt(s['tail_latency_ms']['p50'], 0)} / "
                  f"{_fmt(s['tail_latency_ms']['p95'], 0)}")
    row("总延迟 p50/p95 (ms)",
        lambda s: f"{_fmt(s['total_latency_ms']['p50'], 0)} / "
                  f"{_fmt(s['total_latency_ms']['p95'], 0)}")
    row("跨轮抖动 均值/最差", lambda s: f"{_fmt(s['jitter_mean'], 4)} / {_fmt(s['jitter_max'], 4)}")

    lines += ["", "## 分类别 CER 均值", ""]
    cats = sorted({c for p in report["providers"].values() for c in p["cer_by_category"]})
    lines.append("| 类别 | " + " | ".join(report["providers"].keys()) + " |")
    lines.append("|---|" + "---|" * len(report["providers"]))
    for cat in cats:
        cells = [_fmt(report["providers"][p]["cer_by_category"].get(cat), 4)
                 for p in report["providers"]]
        lines.append(f"| {cat} | " + " | ".join(cells) + " |")

    lines += ["", "## 失败与错误统计", ""]
    for p, s in report["providers"].items():
        if s["error_kinds"]:
            kinds = ", ".join(f"{k}×{v}" for k, v in sorted(s["error_kinds"].items()))
            lines.append(f"- **{p}**：{kinds}")
        else:
            lines.append(f"- **{p}**：无错误")

    worst = sorted((c for c in report["clips"] if c["cer"] is not None),
                   key=lambda c: -c["cer"])[:10]
    lines += ["", "## CER 最差 Top 10（成功用例）", ""]
    lines.append("| 语料 | 平台 | 轮次 | CER | 参考文本 | 识别文本 |")
    lines.append("|---|---|---|---|---|---|")
    for c in worst:
        ref = c["reference"].replace("|", "\\|")
        hyp = c["final_text"].replace("|", "\\|")
        lines.append(f"| {c['clip_id']} | {c['provider']} | {c['round']} | "
                     f"{c['cer']:.4f} | {ref} | {hyp} |")

    failed = [c for c in report["clips"] if not c["success"]]
    if failed:
        lines += ["", "## 失败用例明细", ""]
        lines.append("| 语料 | 平台 | 轮次 | 错误 | 服务端错误 |")
        lines.append("|---|---|---|---|---|")
        for c in failed:
            srv = "; ".join(c["server_errors"]).replace("|", "\\|")[:120]
            lines.append(f"| {c['clip_id']} | {c['provider']} | {c['round']} | "
                         f"{c['error'].replace('|', '\\|')} | {srv} |")

    lines.append("")
    return "\n".join(lines)


def write_reports(report: dict, out_dir: Path, stamp: str) -> tuple[Path, Path]:
    """写 JSON 到 out_dir，返回 (json_path, markdown_path)。"""
    out_dir.mkdir(parents=True, exist_ok=True)
    json_path = out_dir / f"asr_bench_{stamp}.json"
    json_path.write_text(json.dumps(report, ensure_ascii=False, indent=2),
                         encoding="utf-8")
    md_path = out_dir / f"asr_bench_{stamp}.md"
    md_path.write_text(build_markdown(report), encoding="utf-8")
    return json_path, md_path
