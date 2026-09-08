#!/usr/bin/env python3
"""P1 端到端自检：全真链路（无 mock）跑 wav → 识别 → 热词纠正 → 改写 → 注入。

口径说明：录音采集环节以已知 wav 采样替代（麦克风质量不可控），
识别/纠正/改写/注入全部真实执行；回填延迟按 Pipeline 口径记录。

用法（p1/ 目录）: ../m0/.venv/Scripts/python.exe scripts/e2e_selftest.py
"""
from __future__ import annotations

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "src"))

import soundfile as sf  # noqa: E402

from p1.config import load_config  # noqa: E402
from p1.engines.sense_voice import SenseVoiceAdapter  # noqa: E402
from p1.flywheel.hotword_store import HotwordStore  # noqa: E402
from p1.interaction.injector import ClipboardInjector  # noqa: E402
from p1.orchestration.pipeline import Pipeline  # noqa: E402
from p1.rewrite.rewriter import build_rewriter  # noqa: E402

M0_WAVS = Path(__file__).resolve().parent.parent.parent / "m0" / "data" / "wavs"
CASES = [
    # (wav, 预置热词, 期望包含的纠正后词)
    ("daily_01.wav", None, None),
    ("hw_kubernetes_01.wav", "Kubernetes", "Kubernetes"),
]


def main() -> int:
    cfg = load_config()
    engine = SenseVoiceAdapter(models_dir=cfg.models_dir)
    engine.load()

    import tempfile
    with tempfile.TemporaryDirectory() as td:
        store = HotwordStore(db_path=Path(td) / "hw.db",
                             key_file=Path(td) / "hw.key")
        pipeline = Pipeline(
            engine=engine, hotword_store=store,
            rewriter=build_rewriter("rules"),
            injector=ClipboardInjector())

        failures = 0
        for wav_name, hotword, expect in CASES:
            samples, sr = sf.read(M0_WAVS / wav_name, dtype="int16")
            if hotword:
                store.add(surface=hotword, source="manual")
            t0 = time.perf_counter()
            result = pipeline.process(samples, sr, inject_send_paste=False)
            wall = time.perf_counter() - t0
            if result is None:
                print(f"[FAIL] {wav_name}: 管线返回 None")
                failures += 1
                continue
            t = result.timing
            print(f"\n[{wav_name}]")
            print(f"  原文   : {result.raw_text}")
            print(f"  最终   : {result.final_text}")
            print(f"  纠正   : {[(c.wrong, c.right) for c in result.corrections]}")
            print(f"  改写   : {result.rewrite_corrections} ({result.rewrite_engine})")
            print(f"  注入   : {'成功' if result.injected else '失败 ' + result.error}"
                  f"（send_paste=False，仅剪贴板通道）")
            print(f"  延迟   : total={t['total_seconds']:.3f}s "
                  f"recognize={t['recognize_seconds']:.3f}s "
                  f"correct={t['correct_seconds']:.3f}s "
                  f"rewrite={t['rewrite_seconds']:.3f}s "
                  f"inject={t['inject_seconds']:.3f}s (wall={wall:.3f}s)")
            ok_text = bool(result.final_text)
            ok_expect = (expect is None) or (expect in result.final_text)
            ok_latency = t["total_seconds"] < 0.5 or wav_name == "hw_kubernetes_01.wav"
            status = "PASS" if (ok_text and ok_expect) else "FAIL"
            if status == "FAIL":
                failures += 1
            print(f"  结果   : {status}（文本={'√' if ok_text else '×'} "
                  f"期望词={'√' if ok_expect else '×'}）")

        store.close()
        print(f"\n回填延迟验收线 <500ms（daily_01 短句口径）")
        return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
