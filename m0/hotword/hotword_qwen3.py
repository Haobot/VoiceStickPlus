#!/usr/bin/env python3
"""热词验证 · 路线二（提示词 B 任务 4）：Qwen3-ASR context 偏置双跑对比。

同一批 20 句热词音频跑两遍：
  基线 = 无上下文
  偏置 = 热词写进构造级 context（from_qwen3_asr(hotwords="词1,词2,...")，
         Qwen3-ASR 原生 context biasing，官方热词形态为逗号分隔）

注：per-stream create_stream(hotwords=...) 仅对 transducer 模型生效（见
docs/pitfalls.md 坑记录），Qwen3-ASR 走构造级注入——产品形态中等价于
"每次识别前组装热词上下文"（重建轻量 config 即可，权重常驻内存）。

Qwen3-ASR 0.6B int8 为 LLM 解码器架构，纯 CPU 单句延迟显著高于 SenseVoice/SeACo，
延迟如实记录（M0 只验证热词收益方向与量级，不要求本路线满足 RTF 通过线）。

用法（在 m0/ 目录下，需已下载 qwen3_asr 模型）:
    .venv/Scripts/python.exe hotword/hotword_qwen3.py
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

M0_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(M0_ROOT / "src"))

from m0_asr.engine import create_qwen3_recognizer, transcribe  # noqa: E402
from m0_asr.metrics import hotword_hit  # noqa: E402
from hotword_seaco import summarize  # noqa: E402 - 复用同目录工具

TESTSET_JSON = M0_ROOT / "data" / "texts" / "testset.json"
RESULTS_JSON = M0_ROOT / "data" / "results" / "hotword_qwen3.json"


def run_qwen3_suite(recognizer, entries, label: str) -> list[dict]:
    """跑 20 句 Qwen3 识别（热词已在构造时注入）。"""
    details = []
    for entry in entries:
        wav = M0_ROOT / "data" / "wavs" / f"{entry['id']}.wav"
        result = transcribe(recognizer, wav)
        details.append({
            "id": entry["id"],
            "hotword": entry["hotword"],
            "text_gt": entry["text"],
            "text_asr": result.text,
            "elapsed_seconds": round(result.elapsed_seconds, 4),
            "audio_seconds": round(result.audio_seconds, 3),
            "hit": hotword_hit(result.text, entry["hotword"]),
        })
        mark = "✅" if details[-1]["hit"] else "❌"
        print(f"  [{label}] {entry['id']}: {mark} {result.elapsed_seconds:.2f}s | {result.text}")
    return details


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--threads", type=int, default=4)
    args = parser.parse_args()

    testset = json.loads(TESTSET_JSON.read_text(encoding="utf-8"))
    entries = testset["hotword"]
    all_hotwords = sorted({e["hotword"] for e in entries})
    print(f"热词列表（{len(all_hotwords)} 个）: {all_hotwords}\n")

    print("== Qwen3-ASR 基线（无上下文）==")
    t0 = time.perf_counter()
    base_recognizer = create_qwen3_recognizer(num_threads=args.threads)
    print(f"加载 {time.perf_counter() - t0:.1f}s")
    # 首句预热（排除首次推理的初始化开销，让基线/偏置计时公平）
    transcribe(base_recognizer, M0_ROOT / "data" / "wavs" / f"{entries[0]['id']}.wav")
    print("预热完成")
    base_details = run_qwen3_suite(base_recognizer, entries, "基线")
    del base_recognizer  # 释放内存（0.6B 权重 ~1GB 常驻）

    print("\n== Qwen3-ASR 偏置（context 注入 10 热词）==")
    t0 = time.perf_counter()
    biased_recognizer = create_qwen3_recognizer(num_threads=args.threads, hotwords_csv=",".join(all_hotwords))
    print(f"加载 {time.perf_counter() - t0:.1f}s")
    transcribe(biased_recognizer, M0_ROOT / "data" / "wavs" / f"{entries[0]['id']}.wav")
    print("预热完成")
    biased_details = run_qwen3_suite(biased_recognizer, entries, "偏置")

    output = {
        "route": "qwen3_asr",
        "hotwords": all_hotwords,
        "qwen3_no_bias": {"details": base_details, **summarize(base_details)},
        "qwen3_with_bias": {"details": biased_details, **summarize(biased_details)},
        "hardware_note": "同 benchmark.py 所用机器，见 docs/benchmark_report.md",
    }
    RESULTS_JSON.parent.mkdir(parents=True, exist_ok=True)
    RESULTS_JSON.write_text(json.dumps(output, ensure_ascii=False, indent=2), encoding="utf-8")

    base_sum, bias_sum = summarize(base_details), summarize(biased_details)
    reduction = (
        (base_sum["error_rate"] - bias_sum["error_rate"]) / base_sum["error_rate"]
        if base_sum["error_rate"] > 0 else 0.0
    )
    print(f"\n== 汇总 ==")
    print(f"Qwen3 无偏置: 命中 {base_sum['hits']}/{base_sum['total']}，错误率 {base_sum['error_rate']:.1%}")
    print(f"Qwen3 偏置  : 命中 {bias_sum['hits']}/{bias_sum['total']}，错误率 {bias_sum['error_rate']:.1%}")
    print(f"降幅: {reduction:.1%}（M0 通过线 ≥ 50%）")
    print(f"延迟: 无偏置中位 {base_sum['median_elapsed']}s → 偏置中位 {bias_sum['median_elapsed']}s")
    print(f"结果已写入: {RESULTS_JSON}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
