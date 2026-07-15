"""语料生成脚本（M1.1 实现）。

对 corpus/corpus.json 每条语料：
  1. edge-tts 生成中文 mp3（临时）
  2. ffmpeg 转 16kHz/mono/s16le raw PCM  -> {id}.pcm
  3. ffmpeg 转 16kHz/mono Ogg Opus 32kbps voip -> {id}.ogg
  4. 写预期文本 -> {id}.txt

用法：
  python gen_corpus.py [--voice zh-CN-XiaoxiaoNeural] [--ffmpeg PATH]

退出码 0 = 全部生成成功，1 = 有失败。
"""
import argparse
import asyncio
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import edge_tts

CORPUS_DIR = Path(__file__).resolve().parent / "corpus"
DEFAULT_VOICE = "zh-CN-XiaoxiaoNeural"


def find_ffmpeg(explicit: str | None) -> str | None:
    """定位 ffmpeg 可执行文件：优先 --ffmpeg 参数，其次 PATH。"""
    if explicit:
        p = Path(explicit)
        if p.is_file():
            return str(p)
        print(f"WARN: --ffmpeg 指定的路径不存在 {explicit}", file=sys.stderr)
    return shutil.which("ffmpeg")


async def gen_one(item: dict, voice: str, ffmpeg: str, tmpdir: Path) -> None:
    """生成单条语料的三件套产物。失败抛异常。"""
    cid = item["id"]
    text = item["text"]
    mp3 = tmpdir / f"{cid}.mp3"

    # 1. edge-tts -> mp3
    communicate = edge_tts.Communicate(text, voice)
    await communicate.save(str(mp3))
    if not mp3.exists() or mp3.stat().st_size == 0:
        raise RuntimeError("edge-tts 生成空 mp3")

    pcm = CORPUS_DIR / f"{cid}.pcm"
    ogg = CORPUS_DIR / f"{cid}.ogg"

    # 2. mp3 -> raw PCM 16kHz mono s16le
    subprocess.run(
        [ffmpeg, "-y", "-i", str(mp3), "-ar", "16000", "-ac", "1", "-f", "s16le", str(pcm)],
        check=True, capture_output=True,
    )
    if not pcm.exists() or pcm.stat().st_size < 16000:
        raise RuntimeError(f"pcm 生成异常 size={pcm.stat().st_size if pcm.exists() else 0}")

    # 3. mp3 -> Ogg Opus 16kHz mono 32kbps voip（对齐固件编码场景）
    subprocess.run(
        [ffmpeg, "-y", "-i", str(mp3), "-ar", "16000", "-ac", "1",
         "-c:a", "libopus", "-b:a", "32k", "-application", "voip", str(ogg)],
        check=True, capture_output=True,
    )
    if not ogg.exists() or ogg.stat().st_size == 0:
        raise RuntimeError("ogg 生成异常")

    # 4. 写预期文本
    (CORPUS_DIR / f"{cid}.txt").write_text(text, encoding="utf-8")


async def main_async(args: argparse.Namespace) -> int:
    ffmpeg = find_ffmpeg(args.ffmpeg)
    if not ffmpeg:
        print("FAIL: ffmpeg not found. Use --ffmpeg PATH or add to PATH.", file=sys.stderr)
        return 1

    manifest_path = CORPUS_DIR / "corpus.json"
    if not manifest_path.exists():
        print(f"FAIL: corpus manifest not found {manifest_path}", file=sys.stderr)
        return 1
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))

    CORPUS_DIR.mkdir(parents=True, exist_ok=True)
    ok = 0
    failures: list[str] = []
    with tempfile.TemporaryDirectory() as td:
        tmpdir = Path(td)
        for item in manifest:
            try:
                await gen_one(item, args.voice, ffmpeg, tmpdir)
                print(f"  OK   {item['id']}")
                ok += 1
            except Exception as e:  # noqa: BLE001 - 逐条收集失败不中断整体
                msg = f"{item['id']}: {e}"
                print(f"  FAIL {msg}", file=sys.stderr)
                failures.append(msg)

    print(f"done: ok={ok} fail={len(failures)}")
    return 0 if not failures else 1


def main() -> int:
    # 让中文输出在 GBK 控制台下也可读（PowerShell 5.1）
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except Exception:  # noqa: BLE001
        pass
    ap = argparse.ArgumentParser(description="Generate Voice Stick test corpus.")
    ap.add_argument("--voice", default=DEFAULT_VOICE, help=f"edge-tts voice (default {DEFAULT_VOICE})")
    ap.add_argument("--ffmpeg", default=None, help="path to ffmpeg.exe (default: search PATH)")
    args = ap.parse_args()
    return asyncio.run(main_async(args))


if __name__ == "__main__":
    sys.exit(main())
