#!/usr/bin/env python3
"""单文件/麦克风语音识别（提示词 A 任务 3 主入口）。

用法（在 m0/ 目录下）:
    .venv/Scripts/python.exe transcribe.py path/to/audio.wav
    .venv/Scripts/python.exe transcribe.py --mic 10          # 录 10 秒
    .venv/Scripts/python.exe transcribe.py a.wav b.wav       # 批量

输出：识别文本 / 音频时长 / 处理耗时 / RTF（实时系数 = 耗时/音频时长）。

流式接口预留（M0 后续阶段实现，本阶段只做离线整段识别）：
    未来接入 sherpa_onnx.OnlineRecognizer（流式 zipformer）时，
    transcribe() 的「读取→重采样→解码」三步改为分块喂入：
    for chunk in chunked(samples, 0.1 * TARGET_SAMPLE_RATE):
        online_stream.accept_waveform(TARGET_SAMPLE_RATE, chunk)
    并在上层用 VAD 决定句尾；离线引擎（SenseVoice/SeACo/Qwen3）将作为
    「松手后整段重识别」的兜底路径与本流式路径并联。
"""
from __future__ import annotations

import argparse
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "src"))

from m0_asr.engine import create_sense_voice_recognizer, transcribe  # noqa: E402


def record_mic(seconds: float, wav_path: Path) -> None:
    """用系统默认麦克风录制 N 秒，存 16k 单声道 wav。"""
    import numpy as np
    import sounddevice as sd
    import soundfile as sf

    print(f"开始录音 {seconds:.0f} 秒（默认输入设备）...")
    audio = sd.rec(int(seconds * 16000), samplerate=16000, channels=1, dtype="float32")
    sd.wait()
    sf.write(wav_path, audio, 16000)
    print("录音结束。")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("wavs", nargs="*", help="16kHz 单声道 wav 文件路径（其他采样率自动重采样）")
    parser.add_argument("--mic", type=float, metavar="秒", help="实时录音时长（秒）")
    parser.add_argument("--threads", type=int, default=4, help="推理线程数（默认 4）")
    args = parser.parse_args()

    if not args.wavs and args.mic is None:
        parser.error("请提供 wav 文件路径或 --mic 秒数")

    import time

    started = time.perf_counter()
    recognizer = create_sense_voice_recognizer(num_threads=args.threads)
    print(f"[模型加载] {time.perf_counter() - started:.2f}s")

    paths: list[Path] = [Path(w) for w in args.wavs]
    if args.mic is not None:
        tmp = Path(tempfile.gettempdir()) / "voicestick_m0_mic.wav"
        record_mic(args.mic, tmp)
        paths.append(tmp)

    for path in paths:
        result = transcribe(recognizer, path)
        print(f"\n=== {path.name} ===")
        print(f"文本: {result.text}")
        print(f"音频时长: {result.audio_seconds:.2f}s | 处理耗时: {result.elapsed_seconds:.3f}s | RTF: {result.rtf:.3f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
