#!/usr/bin/env python3
"""测试音频生成：edge-tts 合成 mp3 → ffmpeg 转 16k 单声道 wav。

用法（在 m0/ 目录下）:
    .venv/Scripts/python.exe scripts/gen_test_audio.py            # 生成全部（幂等跳过已有）
    .venv/Scripts/python.exe scripts/gen_test_audio.py --force    # 强制重生成

口径说明（写入报告的注意事项）：
- 音频由 edge-tts 神经网络语音合成（非真人录音），发音标准、无背景噪声，
  等价于"安静环境朗读"条件；真实嘈杂环境下的表现不在本测试集覆盖范围。
- ground truth 为 data/texts/testset.json 中人工标注的 text 字段，
  未用任何 ASR 模型输出互相校对。
- 仅音频生成阶段联网（调用微软 TTS 服务）；识别运行时全程离线。
"""
from __future__ import annotations

import argparse
import asyncio
import json
import shutil
import subprocess
import sys
from pathlib import Path

M0_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(M0_ROOT / "src"))

TESTSET_JSON = M0_ROOT / "data" / "texts" / "testset.json"
WAVS_DIR = M0_ROOT / "data" / "wavs"
TTS_CACHE = M0_ROOT / "data" / "tts_cache"


def ffmpeg_to_wav16k(mp3: Path, wav: Path) -> None:
    """mp3 → 16kHz 单声道 16bit wav（ASR 标准输入格式）。"""
    subprocess.run(
        ["ffmpeg", "-y", "-loglevel", "error", "-i", str(mp3),
         "-ar", "16000", "-ac", "1", "-sample_fmt", "s16", str(wav)],
        check=True,
    )


async def synth_one(text: str, voice: str, rate: str, mp3_path: Path) -> None:
    """调用 edge-tts 合成单条 mp3。"""
    import edge_tts

    communicate = edge_tts.Communicate(text, voice, rate=rate)
    await communicate.save(str(mp3_path))


async def main_async(force: bool) -> int:
    testset = json.loads(TESTSET_JSON.read_text(encoding="utf-8"))
    entries = testset["benchmark"] + testset["hotword"]
    WAVS_DIR.mkdir(parents=True, exist_ok=True)
    TTS_CACHE.mkdir(parents=True, exist_ok=True)

    todo = [e for e in entries if force or not (WAVS_DIR / f"{e['id']}.wav").exists()]
    print(f"共 {len(entries)} 条，待生成 {len(todo)} 条")

    for entry in todo:
        wav_path = WAVS_DIR / f"{entry['id']}.wav"
        mp3_path = TTS_CACHE / f"{entry['id']}.mp3"
        rate = "-5%" if "hotword" in entry["id"] else "+0%"
        try:
            await synth_one(entry["text"], entry["voice"], rate, mp3_path)
            ffmpeg_to_wav16k(mp3_path, wav_path)
            print(f"  [ok] {entry['id']}.wav")
        except Exception as exc:  # 单条失败不中断整体
            print(f"  [FAIL] {entry['id']}: {exc}")
            return 1
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--force", action="store_true", help="强制重新生成全部音频")
    args = parser.parse_args()

    if shutil.which("ffmpeg") is None:
        print("错误: 需要 ffmpeg（用于 mp3 → wav 转换），请先安装")
        return 1
    return asyncio.run(main_async(force=args.force))


if __name__ == "__main__":
    sys.exit(main())
