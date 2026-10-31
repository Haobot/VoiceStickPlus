#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""本地 ASR 离线评测：whisper.cpp 系 × sherpa-onnx SenseVoice int8。

口径与云端基线（scripts/e2e_test/run_asr_bench.py，2026-08-01 报告）严格对齐：
- 同一份语料 scripts/e2e_test/corpus（31 条，16kHz s16le mono PCM + 标注 txt）
- 同一套 CER 归一化（直接 import asr_bench.metrics，禁止复制漂移）
- 每引擎 3 轮全量回放，报跨轮抖动（与云端 3 轮口径一致）

延迟口径（本地引擎无流式 partial，与云端不可直接对齐，分开报）：
- sensevoice：进程内常驻引擎（与 Windows local_asr_client_win.cc 同参数：
  language=auto, use_itn=1, num_threads=6），报稳态单条 decode p50/p95
  （不含模型加载），另报冷启动首条延迟。
- whisper：每条独立进程调用。冷墙钟 = 进程墙时（含模型加载）；热耗时 =
  从 stderr whisper_print_timings 解析的 encode+decode 等阶段和。
  常驻进程的稳态延迟近似用热耗时。

用法：
    .venv/bin/python run_asr_bench.py --engines sensevoice whisper-turbo-zh ...
    .venv/bin/python run_asr_bench.py --list-engines
结果写入 results/asr_local_<ts>.{json,md}
"""
from __future__ import annotations

import argparse
import json
import re
import statistics
import struct
import subprocess
import sys
import time
import wave
from dataclasses import dataclass, field, asdict
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(REPO / "scripts" / "e2e_test"))

from asr_bench.metrics import cer  # noqa: E402  云端基线同款打分

CORPUS_DIR = REPO / "scripts" / "e2e_test" / "corpus"
MODELS_DIR = HERE / "models"
RESULTS_DIR = HERE / "results"
WAV_CACHE = HERE / "wav_cache"

ROUNDS = 3

# 云端基线（scripts/e2e_test/bench_results/asr_bench_merged_20260801-025118.md，
# 同语料同口径，31 条 × 3 轮，不带热词）——仅供报告对比引用，不参与计算。
CLOUD_BASELINE = {
    "tencent": {"cer_mean": 0.0089, "cer_worst": 0.1304,
                "first_partial_p50_ms": 672, "tail_p50_ms": 125},
    "volcengine": {"cer_mean": 0.0134, "cer_worst": 0.2609,
                   "first_partial_p50_ms": 4157, "tail_p50_ms": 969},
}

WHISPER_BIN = "whisper-cli"


@dataclass
class ItemResult:
    item_id: str
    category: str
    round: int
    text: str = ""
    ok: bool = True
    error: str = ""
    cold_ms: float | None = None   # 进程墙钟（含加载）；sensevoice 为冷启动首条
    hot_ms: float | None = None    # 纯推理阶段耗时（常驻近似）


@dataclass
class EngineResult:
    engine: str
    backend_note: str = ""
    items: list[ItemResult] = field(default_factory=list)


def load_corpus() -> list[dict]:
    corpus = json.loads((CORPUS_DIR / "corpus.json").read_text(encoding="utf-8"))
    out = []
    for entry in corpus:
        pcm = CORPUS_DIR / f"{entry['id']}.pcm"
        txt = (CORPUS_DIR / f"{entry['id']}.txt").read_text(encoding="utf-8").strip()
        assert pcm.exists(), pcm
        out.append({"id": entry["id"], "category": entry["category"],
                    "reference": txt, "pcm": str(pcm)})
    return out


def pcm_to_wav(pcm_path: Path) -> Path:
    """16kHz s16le mono PCM 加 RIFF 头，供 whisper-cli 使用；结果缓存。"""
    WAV_CACHE.mkdir(exist_ok=True)
    wav = WAV_CACHE / (pcm_path.stem + ".wav")
    if not wav.exists():
        raw = pcm_path.read_bytes()
        with wave.open(str(wav), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(16000)
            w.writeframes(raw)
    return wav


def read_pcm_float(path: Path) -> list[float]:
    import numpy as np
    raw = path.read_bytes()
    return (np.frombuffer(raw, dtype=np.int16).astype(np.float32) / 32768.0).tolist()


# ---------------------------------------------------------------- SenseVoice

def run_sensevoice(corpus: list[dict], rounds: int) -> EngineResult:
    import sherpa_onnx

    model = MODELS_DIR / "sense-voice-int8-2024-07-17" / "model.int8.onnx"
    tokens = MODELS_DIR / "sense-voice-int8-2024-07-17" / "tokens.txt"
    t0 = time.perf_counter()
    rec = sherpa_onnx.OfflineRecognizer.from_sense_voice(
        model=str(model), tokens=str(tokens),
        language="auto", use_itn=True, num_threads=6, debug=False)
    load_ms = (time.perf_counter() - t0) * 1000

    # 预读全部音频（I/O 不计入延迟）
    waves = {it["id"]: read_pcm_float(Path(it["pcm"])) for it in corpus}

    res = EngineResult(
        "sensevoice",
        backend_note=(f"sherpa-onnx {sherpa_onnx.__version__} SenseVoice int8 "
                      f"language=auto use_itn=1 threads=6; model load {load_ms:.0f}ms"))

    first_decode_done = False
    for rnd in range(1, rounds + 1):
        for it in corpus:
            t0 = time.perf_counter()
            stream = rec.create_stream()
            stream.accept_waveform(16000, waves[it["id"]])
            rec.decode_stream(stream)
            dt = (time.perf_counter() - t0) * 1000
            text = re.sub(r"<\|[^|]*\|>", "", stream.result.text).strip()
            ir = ItemResult(it["id"], it["category"], rnd, text=text,
                            hot_ms=dt, cold_ms=(dt if not first_decode_done else None))
            first_decode_done = True
            res.items.append(ir)
    return res


# ---------------------------------------------------------------- whisper.cpp

WHISPER_ENGINES = {
    # name: (model file, language)
    "whisper-tiny-zh": ("ggml-tiny.bin", "zh"),
    "whisper-base-zh": ("ggml-base.bin", "zh"),
    "whisper-small-zh": ("ggml-small.bin", "zh"),
    "whisper-turbo-zh": ("ggml-large-v3-turbo-q5_0.bin", "zh"),
    "whisper-turbo-auto": ("ggml-large-v3-turbo-q5_0.bin", "auto"),
}


def parse_whisper_timings(stderr_text: str) -> float | None:
    """从 whisper_print_timings 输出解析纯推理耗时（total - load，ms）。"""
    total = load = None
    for line in stderr_text.splitlines():
        m = re.search(r"whisper_print_timings:\s+(\S+) time =\s+([0-9.]+)\s*ms", line)
        if not m:
            continue
        kind, val = m.group(1), float(m.group(2))
        if kind == "total":
            total = val
        elif kind == "load":
            load = val
    if total is not None:
        return total - (load or 0.0)
    return None


def run_whisper(corpus: list[dict], rounds: int, name: str) -> EngineResult:
    model_file, lang = WHISPER_ENGINES[name]
    model = MODELS_DIR / model_file
    assert model.exists(), model

    # 探测一次拿 backend 信息（Metal 与否）；不带 -np 以保留 timings
    probe = subprocess.run(
        [WHISPER_BIN, "-m", str(model), "-f", str(pcm_to_wav(Path(corpus[0]["pcm"]))),
         "-l", lang, "-nt", "-t", "8"],
        capture_output=True, text=True, timeout=120)
    sysinfo = probe.stderr
    metal = "ggml_metal_device_init" in sysinfo or "Metal" in sysinfo

    res = EngineResult(name, backend_note=f"whisper.cpp {lang=} threads=8 "
                      f"Metal={'on' if metal else 'off'} quant={model_file}")
    for rnd in range(1, rounds + 1):
        for it in corpus:
            wav = pcm_to_wav(Path(it["pcm"]))
            t0 = time.perf_counter()
            proc = subprocess.run(
                [WHISPER_BIN, "-m", str(model), "-f", str(wav),
                 "-l", lang, "-nt", "-t", "8"],
                capture_output=True, text=True, timeout=180)
            wall = (time.perf_counter() - t0) * 1000
            if proc.returncode != 0:
                res.items.append(ItemResult(
                    it["id"], it["category"], rnd, ok=False,
                    error=proc.stderr.strip()[:200], cold_ms=wall))
                continue
            text = " ".join(ln.strip() for ln in proc.stdout.splitlines()
                            if ln.strip())
            res.items.append(ItemResult(
                it["id"], it["category"], rnd, text=text, cold_ms=wall,
                hot_ms=parse_whisper_timings(proc.stderr)))
    return res


# ---------------------------------------------------------------- 汇总与报告

def summarize(res: EngineResult, corpus: list[dict]) -> dict:
    ok = [i for i in res.items if i.ok]
    fails = [i for i in res.items if not i.ok]
    refs = {c["id"]: c["reference"] for c in corpus}
    cats = {c["id"]: c["category"] for c in corpus}

    cers = [cer(refs[i.item_id], i.text) for i in ok]
    by_cat: dict[str, list[float]] = {}
    for i in ok:
        by_cat.setdefault(cats[i.item_id], []).append(cer(refs[i.item_id], i.text))

    # 跨轮抖动：同 item 不同 round 的 CER 极差
    per_item: dict[str, list[float]] = {}
    for i in ok:
        per_item.setdefault(i.item_id, []).append(cer(refs[i.item_id], i.text))
    jitters = [max(v) - min(v) for v in per_item.values() if len(v) > 1]

    def pct(vals, p):
        if not vals:
            return None
        s = sorted(vals)
        return s[min(len(s) - 1, round(p / 100 * (len(s) - 1)))]

    hot = [i.hot_ms for i in ok if i.hot_ms is not None]
    cold = [i.cold_ms for i in ok if i.cold_ms is not None]

    worst = sorted(ok, key=lambda i: cer(refs[i.item_id], i.text), reverse=True)[:10]

    return {
        "engine": res.engine,
        "backend": res.backend_note,
        "runs": f"{len(ok)}/{len(res.items)}",
        "fail_rate": round(len(fails) / max(1, len(res.items)), 4),
        "cer_mean": round(statistics.mean(cers), 4) if cers else None,
        "cer_median": round(statistics.median(cers), 4) if cers else None,
        "cer_worst": round(max(cers), 4) if cers else None,
        "cer_by_category": {k: round(statistics.mean(v), 4)
                            for k, v in sorted(by_cat.items())},
        "jitter_mean": round(statistics.mean(jitters), 4) if jitters else None,
        "jitter_worst": round(max(jitters), 4) if jitters else None,
        "hot_ms_p50": pct(hot, 50), "hot_ms_p95": pct(hot, 95),
        "cold_ms_p50": pct(cold, 50), "cold_ms_p95": pct(cold, 95),
        "failures": [{"item": i.item_id, "round": i.round, "error": i.error}
                     for i in fails],
        "top_worst": [{
            "item": i.item_id, "round": i.round,
            "cer": round(cer(refs[i.item_id], i.text), 4),
            "reference": refs[i.item_id], "hypothesis": i.text,
        } for i in worst],
    }


def write_report(summaries: list[dict], out_json: Path, out_md: Path) -> None:
    out_json.write_text(
        json.dumps({"generated_at": datetime.now(timezone.utc).isoformat(),
                    "cloud_baseline": CLOUD_BASELINE,
                    "engines": summaries},
                   ensure_ascii=False, indent=1), encoding="utf-8")

    lines = ["# 本地 ASR 离线评测报告", "",
             f"- 生成时间：{datetime.now().strftime('%Y-%m-%d %H:%M')}",
             f"- 语料：{CORPUS_DIR.name}/ 31 条，每引擎 {ROUNDS} 轮全量回放",
             "- CER 口径与云端基线一致（asr_bench.metrics 归一化）", "",
             "## 总览对比（含云端基线引用）", "",
             "| 引擎 | 成功 | 失败率 | CER均值 | CER最差 | 热延迟p50/p95(ms) | 冷墙钟p50(ms) | 抖动均值/最差 |",
             "|---|---|---|---|---|---|---|---|"]
    for s in summaries:
        hot = (f"{s['hot_ms_p50']:.0f} / {s['hot_ms_p95']:.0f}"
               if s["hot_ms_p50"] is not None else "—")
        cold = f"{s['cold_ms_p50']:.0f}" if s["cold_ms_p50"] is not None else "—"
        jit = (f"{s['jitter_mean']:.4f} / {s['jitter_worst']:.4f}"
               if s["jitter_mean"] is not None else "—")
        lines.append(
            f"| {s['engine']} | {s['runs']} | {s['fail_rate']:.4f} | "
            f"{s['cer_mean']:.4f} | {s['cer_worst']:.4f} | {hot} | {cold} | {jit} |")
    lines += [
        f"| (基线) tencent 云端 | 93/93 | 0.0000 | {CLOUD_BASELINE['tencent']['cer_mean']:.4f} "
        f"| {CLOUD_BASELINE['tencent']['cer_worst']:.4f} | 尾延迟 125（流式） | — | 0.0014/0.0435 |",
        f"| (基线) volcengine 云端 | 92/93 | 0.0108 | {CLOUD_BASELINE['volcengine']['cer_mean']:.4f} "
        f"| {CLOUD_BASELINE['volcengine']['cer_worst']:.4f} | 尾延迟 969（流式） | — | 0.0000/0.0000 |",
        "",
        "## 分类别 CER 均值", "",
        "| 类别 | " + " | ".join(s["engine"] for s in summaries) + " |",
        "|---" * (len(summaries) + 1) + "|",
    ]
    all_cats = sorted({c for s in summaries for c in s["cer_by_category"]})
    for cat in all_cats:
        row = [cat] + [
            (f"{s['cer_by_category'].get(cat):.4f}" if cat in s["cer_by_category"] else "—")
            for s in summaries]
        lines.append("| " + " | ".join(row) + " |")

    for s in summaries:
        lines += ["", f"## {s['engine']} 明细", "",
                  f"backend: {s['backend']}", "",
                  "### CER 最差 Top 10", "",
                  "| 语料 | 轮次 | CER | 参考 | 识别 |", "|---|---|---|---|---|"]
        for w in s["top_worst"]:
            lines.append(f"| {w['item']} | {w['round']} | {w['cer']:.4f} "
                         f"| {w['reference'][:30]} | {w['hypothesis'][:30]} |")
        if s["failures"]:
            lines += ["", "### 失败用例", ""]
            for f in s["failures"]:
                lines.append(f"- {f['item']} r{f['round']}: {f['error']}")
    out_md.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--engines", nargs="+", default=["sensevoice"])
    ap.add_argument("--list-engines", action="store_true")
    ap.add_argument("--rounds", type=int, default=ROUNDS)
    args = ap.parse_args()

    engines = dict(sensevoice=None, **WHISPER_ENGINES)
    if args.list_engines:
        print("\n".join(engines))
        return

    corpus = load_corpus()
    summaries = []
    for name in args.engines:
        assert name in engines, f"unknown engine {name}; --list-engines"
        t0 = time.perf_counter()
        if name == "sensevoice":
            res = run_sensevoice(corpus, args.rounds)
        else:
            res = run_whisper(corpus, args.rounds, name)
        s = summarize(res, corpus)
        summaries.append(s)
        print(f"[{name}] cer_mean={s['cer_mean']} worst={s['cer_worst']} "
              f"hot_p50={s['hot_ms_p50']} ({time.perf_counter()-t0:.0f}s)",
              flush=True)

    RESULTS_DIR.mkdir(exist_ok=True)
    ts = datetime.now().strftime("%Y%m%d-%H%M%S")
    out_json = RESULTS_DIR / f"asr_local_{ts}.json"
    out_md = RESULTS_DIR / f"asr_local_{ts}.md"
    write_report(summaries, out_json, out_md)
    print(f"written: {out_json}\n          {out_md}")


if __name__ == "__main__":
    main()
