#!/usr/bin/env python3
"""火山 ASR 配置消融实验：定位「首 partial 延迟 ≈ 音频全长」的配置因素。

基线（run_asr_bench.py）发现火山在当前桌面端配置（result_type=full +
enable_nonstream=true + enable_ddc=true）下首 partial 延迟贴着音频结束才来。
本脚本对代表性语料子集回放多组配置组合，对比各组合的成功数、CER 均值、
首 partial 延迟、尾延迟与跨轮抖动，找出「流式快 + 最终准」兼顾的配置。

用法：
    python run_volc_ablation.py                       # 默认 8 条语料 × 5 组配置 × 2 轮
    python run_volc_ablation.py --rounds 1 --clips short_01,verylong_01   # 冒烟

凭据从 %APPDATA%/VoiceStick/config.toml 读取，不打印不输出。
退出码 0 = 运行完成（个别用例失败不算失败）；2 = 凭据/语料缺失；1 = 无结果。
"""
from __future__ import annotations

import argparse
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from asr_bench import volcengine  # noqa: E402
from asr_bench.metrics import cer, per_clip_detail, summarize  # noqa: E402
from asr_bench.result import ClipResult  # noqa: E402
from asr_bench.wsproto import load_windows_config  # noqa: E402
from run_asr_bench import CORPUS_DIR, load_corpus  # noqa: E402

DEFAULT_OUT = HERE / "bench_results"

DEFAULT_CLIPS = ["short_01", "long_01", "mix_01", "tech_01",
                 "fast_01", "noise_01", "english_01", "verylong_01"]

# 消融配置组合：name -> (result_type, enable_nonstream, enable_ddc)
CONFIGS: dict[str, dict] = {
    "base_full_nonstream_ddc": dict(result_type="full", enable_nonstream=True,
                                    enable_ddc=True),   # 桌面端当前配置（基线）
    "single_nonstream_ddc": dict(result_type="single", enable_nonstream=True,
                                 enable_ddc=True),
    "full_ddc": dict(result_type="full", enable_nonstream=False,
                     enable_ddc=True),
    "single_ddc": dict(result_type="single", enable_nonstream=False,
                       enable_ddc=True),
    "single_plain": dict(result_type="single", enable_nonstream=False,
                         enable_ddc=False),
}


def run_one(item: dict, round_no: int, cfg: dict[str, str], config_name: str,
            timeout: float) -> ClipResult:
    res = volcengine.run_clip(
        item["_ogg"], api_key=cfg["volcengine_api_key"], timeout=timeout,
        duration_s=item["_duration"], clip_id=item["id"], round_no=round_no,
        category=item.get("category", ""), reference=item["text"],
        **CONFIGS[config_name])
    # provider 字段携带配置名，summarize 按配置组合分组统计
    res.provider = f"volcengine:{config_name}"
    return res


def build_report(results: list[ClipResult], *, corpus_size: int, rounds: int,
                 config_names: list[str]) -> dict:
    return {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "experiment": "volcengine config ablation (result_type / enable_nonstream / enable_ddc)",
        "corpus_size": corpus_size,
        "rounds": rounds,
        "configs": {name: CONFIGS[name] for name in config_names},
        "providers": {
            f"volcengine:{name}": summarize(
                [r for r in results if r.provider == f"volcengine:{name}"])
            for name in config_names
        },
        "clips": per_clip_detail(results),
    }


def _fmt(value, digits=0, suffix="") -> str:
    if value is None:
        return "—"
    if isinstance(value, float):
        return f"{value:.{digits}f}{suffix}"
    return f"{value}{suffix}"


def build_markdown(report: dict) -> str:
    config_names = [p.split(":", 1)[1] for p in report["providers"]]
    lines = [
        "# 火山 ASR 配置消融实验报告",
        "",
        f"- 生成时间：{report['generated_at']}",
        f"- 语料规模：{report['corpus_size']} 条代表性语料，每配置组合 {report['rounds']} 轮",
        "- 方法：与 run_asr_bench 相同口径（真实时长实时节奏回放、归一化 CER），"
        "仅改动火山 session 请求的 result_type / enable_nonstream / enable_ddc。",
        "",
        "## 配置组合总览",
        "",
        "| 配置 | result_type | nonstream | ddc | 成功/总数 | CER 均值 | "
        "首 partial p50/p95 (ms) | 尾延迟 p50/p95 (ms) | 跨轮抖动 均值/最差 |",
        "|---|---|---|---|---|---|---|---|---|",
    ]
    for name in config_names:
        c = report["configs"][name]
        s = report["providers"][f"volcengine:{name}"]
        lines.append(
            f"| {name} | {c['result_type']} | {c['enable_nonstream']} | {c['enable_ddc']} "
            f"| {s['success']}/{s['runs']} | {_fmt(s['cer_mean'], 4)} "
            f"| {_fmt(s['first_partial_latency_ms']['p50'])} / "
            f"{_fmt(s['first_partial_latency_ms']['p95'])} "
            f"| {_fmt(s['tail_latency_ms']['p50'])} / "
            f"{_fmt(s['tail_latency_ms']['p95'])} "
            f"| {_fmt(s['jitter_mean'], 4)} / {_fmt(s['jitter_max'], 4)} |")

    lines += ["", "## 逐语料首 partial 延迟（ms，各轮均值）", ""]
    lines.append("| 语料 | " + " | ".join(config_names) + " |")
    lines.append("|---|" + "---|" * len(config_names))
    by_clip: dict[str, dict[str, list[float]]] = {}
    for c in report["clips"]:
        if c["success"] and c["first_partial_latency_ms"] is not None:
            name = c["provider"].split(":", 1)[1]
            by_clip.setdefault(c["clip_id"], {}).setdefault(name, []).append(
                c["first_partial_latency_ms"])
    for clip_id in sorted(by_clip):
        cells = []
        for name in config_names:
            vals = by_clip[clip_id].get(name)
            cells.append(f"{sum(vals) / len(vals):.0f}" if vals else "—")
        lines.append(f"| {clip_id} | " + " | ".join(cells) + " |")

    lines += ["", "## 失败与错误统计", ""]
    for name in config_names:
        s = report["providers"][f"volcengine:{name}"]
        if s["error_kinds"]:
            kinds = ", ".join(f"{k}×{v}" for k, v in sorted(s["error_kinds"].items()))
            lines.append(f"- **{name}**：{kinds}")
        else:
            lines.append(f"- **{name}**：无错误")

    failed = [c for c in report["clips"] if not c["success"]]
    if failed:
        lines += ["", "## 失败用例明细", ""]
        lines.append("| 语料 | 配置 | 轮次 | 错误 | 服务端错误 |")
        lines.append("|---|---|---|---|---|")
        for c in failed:
            srv = "; ".join(c["server_errors"]).replace("|", "\\|")[:120]
            lines.append(f"| {c['clip_id']} | {c['provider'].split(':', 1)[1]} | "
                         f"{c['round']} | {c['error'].replace('|', '\\|')} | {srv} |")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except Exception:  # noqa: BLE001
        pass
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--rounds", type=int, default=2, help="每配置组合回放轮数（默认 2）")
    ap.add_argument("--clips", default=",".join(DEFAULT_CLIPS),
                    help="语料子集，逗号分隔 id（默认 8 条代表性语料）")
    ap.add_argument("--configs", default=",".join(CONFIGS.keys()),
                    help="配置组合子集，逗号分隔（默认全部 5 组）")
    ap.add_argument("--timeout", type=float, default=30.0, help="单条等待结果超时秒数")
    ap.add_argument("--out", default=str(DEFAULT_OUT), help="报告输出目录")
    ap.add_argument("--stamp", default=datetime.now().strftime("%Y%m%d-%H%M%S"))
    args = ap.parse_args()

    cfg = load_windows_config()
    if not cfg.get("volcengine_api_key"):
        print("缺少凭据: volcengine_api_key（config.toml）", file=sys.stderr)
        return 2

    config_names = [c.strip() for c in args.configs.split(",") if c.strip()]
    unknown = [c for c in config_names if c not in CONFIGS]
    if unknown:
        print(f"未知配置组合: {', '.join(unknown)}（可选: {', '.join(CONFIGS)}）",
              file=sys.stderr)
        return 2

    only = {c.strip() for c in args.clips.split(",") if c.strip()} or None
    corpus = load_corpus(only)
    if not corpus:
        print("语料为空", file=sys.stderr)
        return 2

    results: list[ClipResult] = []
    total = len(config_names) * args.rounds * len(corpus)
    done = 0
    for config_name in config_names:
        for round_no in range(1, args.rounds + 1):
            for item in corpus:
                done += 1
                res = run_one(item, round_no, cfg, config_name, args.timeout)
                results.append(res)
                tag = "OK " if res.success else "FAIL"
                note = ""
                if res.success:
                    note = f" cer={cer(res.reference, res.final_text):.3f}"
                    if res.first_partial_latency_ms is not None:
                        note += f" first={res.first_partial_latency_ms:.0f}ms"
                    if res.tail_latency_ms is not None:
                        note += f" tail={res.tail_latency_ms:.0f}ms"
                else:
                    note = f" err={res.error}"
                print(f"[{done}/{total}] {tag} {config_name} r{round_no} "
                      f"{item['id']}{note}", flush=True)

    if not results:
        print("无任何结果", file=sys.stderr)
        return 1

    report = build_report(results, corpus_size=len(corpus), rounds=args.rounds,
                          config_names=config_names)
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    json_path = out_dir / f"volc_ablation_{args.stamp}.json"
    json_path.write_text(json.dumps(report, ensure_ascii=False, indent=2),
                         encoding="utf-8")
    md_path = out_dir / f"volc_ablation_{args.stamp}.md"
    md_path.write_text(build_markdown(report), encoding="utf-8")
    print(f"\nJSON: {json_path}")
    print(f"MD:   {md_path}")
    for name in config_names:
        s = report["providers"][f"volcengine:{name}"]
        print(f"{name}: success={s['success']}/{s['runs']} cer_mean={s['cer_mean']} "
              f"first_p50={_fmt(s['first_partial_latency_ms']['p50'], 0, 'ms')} "
              f"tail_p50={_fmt(s['tail_latency_ms']['p50'], 0, 'ms')} "
              f"jitter_max={s['jitter_max']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
