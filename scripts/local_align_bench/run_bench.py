#!/usr/bin/env python3
"""targeted 目标词本地对齐——闭环评测 runner。

闭环链路（与 macOS 集成原型完全同构）：
  dataset.json → align_cli（真实 VoiceTargetedAlign.candidates + chatRequestBody）
  → ollama /api/chat（think:false + JSON-schema enum 硬约束 + temperature 0）
  → parseAnswer 同口径解析 → 评分（对齐命中 / NONE 正确率 / 危险误选 / 延迟）
  → results.json + report.md（含逐例归因）。

用法：python3 run_bench.py [--models m1 m2 ...] [--reps 1] [--api http://127.0.0.1:11434]
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

HERE = Path(__file__).parent
CLI = HERE / "cli" / "align_cli"


def load_dataset() -> list[dict]:
    raw = json.loads((HERE / "dataset.json").read_text(encoding="utf-8"))
    return raw["cases"]


def gen_inputs(cases: list[dict], model: str) -> dict[str, dict]:
    stdin = "\n".join(json.dumps(
        {"id": c["id"], "command": c["command"], "target": c["target"],
         "remaining": c["remaining"]}, ensure_ascii=False) for c in cases)
    proc = subprocess.run([str(CLI), model], input=stdin.encode("utf-8"),
                          capture_output=True, check=True)
    out = {}
    for line in proc.stdout.decode("utf-8").splitlines():
        if line.strip():
            row = json.loads(line)
            out[row["id"]] = row
    return out


def parse_answer(raw: str, candidates: list[str]) -> str | None:
    """与 VoiceTargetedAlign.parseAnswer 同口径。"""
    try:
        obj = json.loads(raw.strip())
        if isinstance(obj, dict) and "answer" in obj:
            a = obj["answer"]
            if a in ("NONE", "none"):
                return "NONE"
            return a if a in candidates else None
    except (json.JSONDecodeError, ValueError):
        pass
    text = raw.strip()
    for a, b in [('"', '"'), ("「", "」"), ("‘", "’"), ("“", "”")]:
        if len(text) >= 2 and text.startswith(a) and text.endswith(b):
            text = text[1:-1].strip()
    if text in ("NONE", "none"):
        return "NONE"
    return text if text in candidates else None


def chat(api: str, body: dict, timeout: float) -> tuple[str | None, int, str | None]:
    data = json.dumps(body, ensure_ascii=False).encode("utf-8")
    req = urllib.request.Request(
        api.rstrip("/") + "/api/chat", data=data,
        headers={"Content-Type": "application/json"})
    started = time.monotonic()
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            payload = json.loads(resp.read().decode("utf-8"))
        content = payload.get("message", {}).get("content", "")
        return content, int((time.monotonic() - started) * 1000), None
    except Exception as exc:  # noqa: BLE001
        return None, int((time.monotonic() - started) * 1000), f"{type(exc).__name__}: {exc}"


def run_model(model: str, cases: list[dict], api: str, reps: int) -> dict:
    inputs = gen_inputs(cases, model)
    # 预热：同一请求两次（首轮含模型加载）。
    first = next(iter(inputs.values()))
    for _ in range(2):
        chat(api, first["request"], timeout=300)
    rows = []
    for c in cases:
        row = inputs[c["id"]]
        cands = [x["text"] for x in row["candidates"]]
        expected = c["expected"]
        record = {
            "id": c["id"], "kind": c["kind"], "target": c["target"],
            "expected": expected, "candidates": cands,
            "prefilter_hit": expected in cands,
            "answer": None, "latency_ms": [], "error": None, "invalid": False,
        }
        answers = []
        for _ in range(reps):
            content, ms, err = chat(api, row["request"], timeout=60)
            record["latency_ms"].append(ms)
            if err:
                record["error"] = err
                continue
            answers.append(parse_answer(content, cands))
        valid = [a for a in answers if a is not None]
        record["invalid"] = bool(answers) and not valid
        record["answer"] = max(set(valid), key=valid.count) if valid else (
            "INVALID" if answers else "ERROR")
        rows.append(record)
    return summarize(model, rows)


def summarize(model: str, rows: list[dict]) -> dict:
    scored = [r for r in rows if r["kind"] not in ("gate", "poisoned")]
    def pick(kind: str) -> list[dict]:
        return [r for r in scored if (kind == "pos" and r["expected"] != "NONE")]

    pos = pick("pos")
    neg = [r for r in scored if r["expected"] == "NONE"]
    pos_prefilter = [r for r in pos if r["prefilter_hit"]]
    lat = sorted(ms for r in rows for ms in r["latency_ms"])

    def med(xs):
        return xs[len(xs) // 2] if xs else None

    def p95(xs):
        return xs[min(len(xs) - 1, int(len(xs) * 0.95))] if xs else None

    wrong_picks = [r for r in rows if r["answer"] not in (None, "ERROR", "INVALID", "NONE")
                   and r["answer"] != r["expected"]]
    return {
        "model": model,
        "rows": rows,
        "pos_total": len(pos),
        "prefilter_recall": f"{len(pos_prefilter)}/{len(pos)}",
        "alignment_correct": sum(r["answer"] == r["expected"] for r in pos_prefilter),
        "alignment_denom": len(pos_prefilter),
        "none_correct": sum(r["answer"] == "NONE" for r in neg),
        "none_total": len(neg),
        "poisoned_none": sum(
            r["answer"] == "NONE"
            for r in rows if r["kind"] == "poisoned"),
        "poisoned_total": sum(1 for r in rows if r["kind"] == "poisoned"),
        "wrong_pick": len(wrong_picks),
        "wrong_pick_ids": [r["id"] for r in wrong_picks],
        "invalid_or_error": sum(r["answer"] in ("INVALID", "ERROR") for r in rows),
        "latency_med_ms": med(lat),
        "latency_p95_ms": p95(lat),
    }


def render_report(results: list[dict], path: Path) -> None:
    lines = [
        "# targeted 目标词本地对齐——多模型闭环评测",
        "",
        "| 模型 | 正例对齐 | 预筛召回 | NONE 正确 | 危险误选 | 无效/错 | 延迟中位/p95 (ms) |",
        "|---|---|---|---|---|---|---|",
    ]
    for r in results:
        lines.append(
            f"| {r['model']} | {r['alignment_correct']}/{r['alignment_denom']}"
            f" ({100 * r['alignment_correct'] / max(1, r['alignment_denom']):.0f}%)"
            f" | {r['prefilter_recall']}"
            f" | {r['none_correct']}/{r['none_total']}"
            f" | {r['wrong_pick']}{' ← ' + ','.join(r['wrong_pick_ids']) if r['wrong_pick_ids'] else ''}"
            f" | {r['invalid_or_error']} | {r['latency_med_ms']}/{r['latency_p95_ms']} |")
    lines += ["", "## 逐例（非满分模型）", ""]
    for r in results:
        bad = [x for x in r["rows"] if x["answer"] != x["expected"]
               or (x["expected"] != "NONE" and not x["prefilter_hit"])]
        if not bad:
            continue
        lines.append(f"### {r['model']}")
        for x in bad:
            mark = "预筛漏" if not x["prefilter_hit"] and x["expected"] != "NONE" else "答错"
            lines.append(
                f"- [{mark}] {x['id']} 目标={x['target']} 期望={x['expected']}"
                f" 答={x['answer']} 候选={x['candidates']}"
                + (f" err={x['error']}" if x["error"] else ""))
        lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--models", nargs="+", required=True)
    ap.add_argument("--reps", type=int, default=1)
    ap.add_argument("--api", default="http://127.0.0.1:11434")
    ap.add_argument("--out", default=str(HERE / "out"))
    args = ap.parse_args()

    cases = load_dataset()
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    results = []
    for model in args.models:
        print(f"=== {model} ...", flush=True)
        results.append(run_model(model, cases, args.api, args.reps))
        r = results[-1]
        print(f"    align {r['alignment_correct']}/{r['alignment_denom']}, "
              f"none {r['none_correct']}/{r['none_total']}, wrong {r['wrong_pick']}, "
              f"med {r['latency_med_ms']}ms", flush=True)
    (out_dir / "results.json").write_text(
        json.dumps(results, ensure_ascii=False, indent=1), encoding="utf-8")
    render_report(results, out_dir / "report.md")
    print(f"report: {out_dir / 'report.md'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
