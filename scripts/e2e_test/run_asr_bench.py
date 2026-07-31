#!/usr/bin/env python3
"""ASR 离线评测基准运行器：对腾讯/火山实时 ASR 做客观对比评估与压力测试。

对 corpus/corpus.json 全部语料按真实时长实时节奏回放两家 ASR WebSocket，
逐条采集最终识别文本、首 partial 延迟、尾延迟、错误/超时/断连事件，
多轮全量回放（默认 3 轮）构成压力/稳定性测试，输出 JSON + Markdown 报告。

用法：
    python run_asr_bench.py --provider all                 # 全量评测（默认 3 轮）
    python run_asr_bench.py --provider volcengine --rounds 1 --clips short_01,mix_01
    python run_asr_bench.py --provider tencent --rounds 1   # 快速验证

凭据从 %APPDATA%/VoiceStick/config.toml 读取，不打印不输出。
退出码 0 = 运行完成（个别用例失败不算失败）；2 = 凭据/语料缺失；1 = 无结果。
"""
from __future__ import annotations

import argparse
import sys
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from asr_bench import tencent, volcengine  # noqa: E402
from asr_bench.report import build_json_report, write_reports  # noqa: E402
from asr_bench.result import ClipResult  # noqa: E402
from asr_bench.wsproto import load_windows_config  # noqa: E402

CORPUS_DIR = HERE / "corpus"
DEFAULT_OUT = HERE / "bench_results"


def load_corpus(only: set[str] | None) -> list[dict]:
    import json
    manifest = json.loads((CORPUS_DIR / "corpus.json").read_text(encoding="utf-8"))
    items = []
    for item in manifest:
        if only and item["id"] not in only:
            continue
        ogg = CORPUS_DIR / f"{item['id']}.ogg"
        pcm = CORPUS_DIR / f"{item['id']}.pcm"
        if not ogg.exists() or not pcm.exists():
            print(f"WARN: {item['id']} 缺少 ogg/pcm，跳过（先跑 gen_corpus.py）",
                  file=sys.stderr)
            continue
        item["_ogg"] = ogg
        # 时长从 PCM 精确计算（16kHz 16bit mono = 32000 B/s）
        item["_duration"] = pcm.stat().st_size / 32000.0
        items.append(item)
    return items


def run_one(provider: str, item: dict, round_no: int, cfg: dict[str, str],
            timeout: float) -> ClipResult:
    common = dict(duration_s=item["_duration"], clip_id=item["id"], round_no=round_no,
                  category=item.get("category", ""), reference=item["text"])
    if provider == "volcengine":
        return volcengine.run_clip(item["_ogg"], api_key=cfg["volcengine_api_key"],
                                   timeout=timeout, **common)
    if provider == "tencent":
        return tencent.run_clip(item["_ogg"], secret_id=cfg["tencent_secret_id"],
                                secret_key=cfg["tencent_secret_key"],
                                appid=cfg["tencent_appid"], timeout=timeout, **common)
    raise ValueError(f"unknown provider {provider}")


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except Exception:  # noqa: BLE001
        pass
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--provider", choices=["all", "volcengine", "tencent"], default="all")
    ap.add_argument("--rounds", type=int, default=3, help="全量回放轮数（压力测试，默认 3）")
    ap.add_argument("--clips", default="", help="只跑指定语料，逗号分隔 id")
    ap.add_argument("--timeout", type=float, default=30.0, help="单条等待结果超时秒数")
    ap.add_argument("--out", default=str(DEFAULT_OUT), help="报告输出目录")
    ap.add_argument("--stamp", default=datetime.now().strftime("%Y%m%d-%H%M%S"))
    args = ap.parse_args()

    cfg = load_windows_config()
    providers = ["volcengine", "tencent"] if args.provider == "all" else [args.provider]
    missing = []
    if "volcengine" in providers and not cfg.get("volcengine_api_key"):
        missing.append("volcengine_api_key")
    if "tencent" in providers:
        for k in ("tencent_secret_id", "tencent_secret_key", "tencent_appid"):
            if not cfg.get(k):
                missing.append(k)
    if missing:
        print(f"缺少凭据: {', '.join(missing)}（config.toml）", file=sys.stderr)
        return 2

    only = {c.strip() for c in args.clips.split(",") if c.strip()} or None
    corpus = load_corpus(only)
    if not corpus:
        print("语料为空", file=sys.stderr)
        return 2

    results: list[ClipResult] = []
    total = len(providers) * args.rounds * len(corpus)
    done = 0
    for provider in providers:
        for round_no in range(1, args.rounds + 1):
            for item in corpus:
                done += 1
                res = run_one(provider, item, round_no, cfg, args.timeout)
                results.append(res)
                tag = "OK " if res.success else "FAIL"
                cer_note = ""
                if res.success:
                    from asr_bench.metrics import cer
                    cer_note = f" cer={cer(res.reference, res.final_text):.3f}"
                    if res.tail_latency_ms is not None:
                        cer_note += f" tail={res.tail_latency_ms:.0f}ms"
                else:
                    cer_note = f" err={res.error}"
                print(f"[{done}/{total}] {tag} {provider} r{round_no} "
                      f"{item['id']}{cer_note}", flush=True)

    if not results:
        print("无任何结果", file=sys.stderr)
        return 1

    report = build_json_report(results, corpus_size=len(corpus), rounds=args.rounds)
    json_path, md_path = write_reports(report, Path(args.out), args.stamp)
    print(f"\nJSON: {json_path}")
    print(f"MD:   {md_path}")
    def _fmt_ms(value) -> str:
        return f"{value:.0f}ms" if isinstance(value, (int, float)) else "—"

    for p, s in report["providers"].items():
        print(f"{p}: success={s['success']}/{s['runs']} "
              f"cer_mean={s['cer_mean']} "
              f"tail_p50={_fmt_ms(s['tail_latency_ms']['p50'])} "
              f"jitter_max={s['jitter_max']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
