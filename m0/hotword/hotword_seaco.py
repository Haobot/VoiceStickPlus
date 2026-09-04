#!/usr/bin/env python3
"""热词验证 · 路线一（提示词 B 任务 3）：FunASR SeACo-Paraformer 热词偏置双跑对比。

同一批 20 句热词音频跑两遍：
  基线 = 无热词
  偏置 = 10 个热词经 generate(hotword=...) 注入（SeACo 神经网络上下文偏置）

同时跑 SenseVoice 无热词基线（主引擎参照，说明热词路线与主引擎的共存关系）。

重要工程结论（本脚本第一版的坑，见 docs/pitfalls.md）：
  sherpa-onnx 导出的 trilingual SeACo 模型**不支持**热词偏置——sherpa-onnx 的
  Aho-Corasick 热词仅实现于 transducer 模型；SeACo 偏置必须走 FunASR 官方
  运行时（本脚本）。两栈并存验证了路线图 §6.2 EngineAdapter 插件化的必要性。

计时口径：funasr generate 为端到端（特征+前向）；基线/偏置双跑同口径，
降幅对比公平；与 SenseVoice 的绝对延迟对比见报告口径说明。

统计口径：实例级热词命中错误率（每句热词恰出现一次，20 实例）。

用法（在 m0/ 目录下，需已下载 funasr_seaco 模型）:
    .venv/Scripts/python.exe hotword/hotword_seaco.py
"""
from __future__ import annotations

import argparse
import json
import statistics
import sys
import time
from pathlib import Path

M0_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(M0_ROOT / "src"))

from m0_asr.audio_utils import read_wave, resample  # noqa: E402
from m0_asr.engine import create_sense_voice_recognizer, transcribe as sv_transcribe  # noqa: E402
from m0_asr.metrics import hotword_hit  # noqa: E402
from m0_asr.model_registry import FUNASR_SEACO  # noqa: E402

TESTSET_JSON = M0_ROOT / "data" / "texts" / "testset.json"
RESULTS_JSON = M0_ROOT / "data" / "results" / "hotword_seaco.json"
TARGET_SAMPLE_RATE = 16000


def run_suite(recognizer, entries, label: str, use_funasr: bool,
              funasr_hotword: str | None = None, sherpa_hotwords: list[str] | None = None) -> list[dict]:
    """跑 20 句，返回逐句明细（文本 + 耗时 + 命中）。

    use_funasr=True: recognizer 为 funasr.AutoModel，热词经 funasr_hotword
    （空格分隔字符串）注入 generate；
    use_funasr=False: recognizer 为 sherpa OfflineRecognizer，热词经
    sherpa_hotwords（列表，内部 `/` 连接）注入 create_stream。
    """
    details = []
    for entry in entries:
        wav = M0_ROOT / "data" / "wavs" / f"{entry['id']}.wav"
        if use_funasr:
            # funasr 直接吃 wav 路径；计时含其内部读取与特征提取
            started = time.perf_counter()
            res = recognizer.generate(input=str(wav), hotword=funasr_hotword)
            elapsed = time.perf_counter() - started
            text = res[0]["text"]
            samples, sr = read_wave(wav)
            audio_seconds = samples.size / sr
        else:
            result = sv_transcribe(recognizer, wav, hotwords=sherpa_hotwords)
            elapsed, text, audio_seconds = result.elapsed_seconds, result.text, result.audio_seconds

        details.append({
            "id": entry["id"],
            "hotword": entry["hotword"],
            "text_gt": entry["text"],
            "text_asr": text,
            "elapsed_seconds": round(elapsed, 4),
            "audio_seconds": round(audio_seconds, 3),
            "hit": hotword_hit(text, entry["hotword"]),
        })
        mark = "✅" if details[-1]["hit"] else "❌"
        print(f"  [{label}] {entry['id']}: {mark} {elapsed:.2f}s | {text}")
    return details


def summarize(details: list[dict]) -> dict:
    total = len(details)
    hits = sum(1 for d in details if d["hit"])
    elapsed = [d["elapsed_seconds"] for d in details]
    audio = [d["audio_seconds"] for d in details]
    return {
        "total": total,
        "hits": hits,
        "misses": total - hits,
        "error_rate": round((total - hits) / total, 4) if total else 0.0,
        "median_elapsed": round(statistics.median(elapsed), 4),
        "total_audio_seconds": round(sum(audio), 2),
        "median_rtf": round(statistics.median(e / a for e, a in zip(elapsed, audio)), 4),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    args = parser.parse_args()

    if not FUNASR_SEACO.is_ready():
        print(f"FunASR SeACo 模型未就位（{FUNASR_SEACO.dir}）。")
        print(f"请先运行: .venv/Scripts/python.exe scripts/download_models.py --only funasr_seaco")
        return 1

    testset = json.loads(TESTSET_JSON.read_text(encoding="utf-8"))
    entries = testset["hotword"]
    all_hotwords = sorted({e["hotword"] for e in entries})
    print(f"热词列表（{len(all_hotwords)} 个）: {all_hotwords}\n")

    # SenseVoice 主引擎无热词基线（参照系）
    print("== SenseVoice 基线（sherpa-onnx，无热词能力，主引擎参照）==")
    t0 = time.perf_counter()
    sv = create_sense_voice_recognizer(num_threads=4)
    print(f"加载 {time.perf_counter() - t0:.1f}s")
    sv_details = run_suite(sv, entries, "SV", use_funasr=False)

    # FunASR SeACo 双跑
    from funasr import AutoModel

    print("\n== 加载 FunASR SeACo-Paraformer（pytorch，本地权重）==")
    t0 = time.perf_counter()
    model = AutoModel(model=str(FUNASR_SEACO.dir), disable_update=True)
    print(f"加载 {time.perf_counter() - t0:.1f}s")

    print("\n== SeACo 基线（无热词）==")
    base_details = run_suite(model, entries, "基线", use_funasr=True)

    print("\n== SeACo 偏置（注入 10 热词）==")
    biased_details = run_suite(model, entries, "偏置", use_funasr=True, funasr_hotword=" ".join(all_hotwords))

    output = {
        "route": "seaco_paraformer_funasr",
        "hotwords": all_hotwords,
        "sense_voice_baseline": {"details": sv_details, **summarize(sv_details)},
        "seaco_no_bias": {"details": base_details, **summarize(base_details)},
        "seaco_with_bias": {"details": biased_details, **summarize(biased_details)},
        "timing_note": "funasr 端到端口径（含特征提取），SenseVoice 仅 decode 口径，绝对值不可直接比",
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
    print(f"SeACo 无偏置: 命中 {base_sum['hits']}/{base_sum['total']}，错误率 {base_sum['error_rate']:.1%}")
    print(f"SeACo 偏置  : 命中 {bias_sum['hits']}/{bias_sum['total']}，错误率 {bias_sum['error_rate']:.1%}")
    print(f"降幅: {reduction:.1%}（M0 通过线 ≥ 50%）")
    print(f"延迟: 无偏置中位 {base_sum['median_elapsed']}s → 偏置中位 {bias_sum['median_elapsed']}s")
    print(f"结果已写入: {RESULTS_JSON}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
