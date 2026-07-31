#!/usr/bin/env python3
"""合并多次 run_asr_bench 的结果 JSON，按 provider 选取来源，重新生成汇总报告。

场景：全量跑中某平台因本机网络抖动大面积失败（如 DNS 故障），网络恢复后单独
重跑该平台，再把两次结果合并为一份干净基线。

用法：
    python merge_bench_results.py --volcengine A.json --tencent B.json [--out DIR]

未指定的 provider 忽略。输出 asr_bench_merged_<时间戳>.json/.md 到 --out
（默认取第一个输入所在目录）。
"""
from __future__ import annotations

import argparse
import sys
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import json  # noqa: E402

from asr_bench.report import build_json_report, write_reports  # noqa: E402
from asr_bench.result import ClipResult  # noqa: E402


def load_results(path: Path, provider: str) -> list[ClipResult]:
    data = json.loads(path.read_text(encoding="utf-8"))
    out = []
    for c in data["clips"]:
        if c["provider"] != provider:
            continue
        out.append(ClipResult(
            clip_id=c["clip_id"], provider=c["provider"], round=c["round"],
            category=c["category"], reference=c["reference"],
            duration_s=c.get("duration_s", 0.0),
            success=c["success"], error=c["error"], final_text=c["final_text"],
            first_partial_latency_ms=c["first_partial_latency_ms"],
            tail_latency_ms=c["tail_latency_ms"],
            total_latency_ms=c.get("total_latency_ms"),
            server_errors=c["server_errors"],
        ))
    return out


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:  # noqa: BLE001
        pass
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--volcengine", default="", help="火山结果来源 JSON")
    ap.add_argument("--tencent", default="", help="腾讯结果来源 JSON")
    ap.add_argument("--out", default="")
    args = ap.parse_args()

    results: list[ClipResult] = []
    first_path = None
    for provider, p in (("volcengine", args.volcengine), ("tencent", args.tencent)):
        if not p:
            continue
        path = Path(p)
        first_path = first_path or path
        got = load_results(path, provider)
        print(f"{provider}: {len(got)} 条结果来自 {path.name}")
        results.extend(got)
    if not results:
        print("未指定任何来源", file=sys.stderr)
        return 2

    rounds = max(r.round for r in results)
    corpus_size = len({r.clip_id for r in results})
    report = build_json_report(results, corpus_size=corpus_size, rounds=rounds)
    out_dir = Path(args.out) if args.out else first_path.parent
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    json_path, md_path = write_reports(report, out_dir, f"merged_{stamp}")
    print(f"JSON: {json_path}")
    print(f"MD:   {md_path}")
    for p, s in report["providers"].items():
        print(f"{p}: success={s['success']}/{s['runs']} cer_mean={s['cer_mean']} "
              f"tail_p50={s['tail_latency_ms']['p50']}ms jitter_max={s['jitter_max']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
