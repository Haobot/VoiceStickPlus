#!/usr/bin/env python3
"""基准测试（提示词 A 任务 4）：3 类音频 × 3 条 × 每条 3 次取中位数。

输出：docs/benchmark_report.md 的 RTF 部分 + 识别结果缓存
（data/results/benchmark_asr.json，供 eval.py 复用，避免重复推理）。

用法（在 m0/ 目录下）:
    .venv/Scripts/python.exe benchmark.py [--runs 3] [--threads 4]
"""
from __future__ import annotations

import argparse
import json
import platform
import statistics
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "src"))

from m0_asr.engine import create_sense_voice_recognizer, transcribe  # noqa: E402

M0_ROOT = Path(__file__).resolve().parent
TESTSET_JSON = M0_ROOT / "data" / "texts" / "testset.json"
RESULTS_JSON = M0_ROOT / "data" / "results" / "benchmark_asr.json"
REPORT_MD = M0_ROOT / "docs" / "benchmark_report.md"

CATEGORY_LABELS = {
    "daily_zh": "①日常对话中文",
    "mix_zh_en": "②中英混读（含技术词）",
    "reading_zh": "③安静环境朗读",
}


def cpu_model() -> str:
    """Windows 下从注册表拿 CPU 型号名。"""
    try:
        import winreg
        with winreg.OpenKey(
            winreg.HKEY_LOCAL_MACHINE,
            r"HARDWARE\DESCRIPTION\System\CentralProcessor\0",
        ) as key:
            return winreg.QueryValueEx(key, "ProcessorNameString")[0]
    except OSError:
        return platform.processor() or "未知"


def hardware_info() -> dict:
    return {
        "cpu": cpu_model(),
        "logical_cores": os_cpu_count(),
        "machine": f"{platform.machine()} / {platform.system()} {platform.release()}",
        "python": platform.python_version(),
    }


def os_cpu_count() -> int:
    import os
    return os.cpu_count() or 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--runs", type=int, default=3, help="每条音频重复识别次数（取中位数）")
    parser.add_argument("--threads", type=int, default=4, help="推理线程数")
    args = parser.parse_args()

    testset = json.loads(TESTSET_JSON.read_text(encoding="utf-8"))
    entries = testset["benchmark"]

    print(f"加载 SenseVoice（threads={args.threads}）...")
    load_started = time.perf_counter()
    recognizer = create_sense_voice_recognizer(num_threads=args.threads)
    load_seconds = time.perf_counter() - load_started
    print(f"模型加载 {load_seconds:.2f}s")

    results = []
    for entry in entries:
        wav = M0_ROOT / "data" / "wavs" / f"{entry['id']}.wav"
        if not wav.exists():
            print(f"[跳过] 缺音频: {wav}")
            continue
        # 第一次跑拿文本，之后重复跑只取耗时
        first = transcribe(recognizer, wav)
        elapsed_list = [first.elapsed_seconds] + [
            transcribe(recognizer, wav).elapsed_seconds for _ in range(args.runs - 1)
        ]
        median_elapsed = statistics.median(elapsed_list)
        results.append({
            "id": entry["id"],
            "category": entry["category"],
            "audio_seconds": round(first.audio_seconds, 3),
            "elapsed_seconds_runs": [round(x, 4) for x in elapsed_list],
            "median_elapsed": round(median_elapsed, 4),
            "median_rtf": round(median_elapsed / first.audio_seconds, 4),
            "text_asr": first.text,
            "text_gt": entry["text"],
        })
        print(f"  {entry['id']}: {first.audio_seconds:.1f}s 中位耗时 {median_elapsed:.3f}s RTF {median_elapsed / first.audio_seconds:.3f}")

    RESULTS_JSON.parent.mkdir(parents=True, exist_ok=True)
    RESULTS_JSON.write_text(
        json.dumps({
            "engine": "sense_voice_int8",
            "runs": args.runs,
            "threads": args.threads,
            "load_seconds": round(load_seconds, 2),
            "hardware": hardware_info(),
            "results": results,
        }, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    print(f"\n识别结果已缓存: {RESULTS_JSON}")
    print("下一步运行 eval.py 计算 CER 并生成完整报告")
    return 0


if __name__ == "__main__":
    sys.exit(main())
