"""语料生成脚本（M1.1 实现，ASR bench 扩充版）。

对 corpus/corpus.json 每条语料：
  1. edge-tts 生成 mp3（临时），支持每条语料可选的 voice / rate 覆盖
  2. 可选按 noise_snr_db 混入白噪声（近似 SNR，仅用于压力测试）
  3. ffmpeg 转 16kHz/mono/s16le raw PCM  -> {id}.pcm
  4. ffmpeg 转 16kHz/mono Ogg Opus 32kbps voip -> {id}.ogg
  5. 写预期文本 -> {id}.txt

corpus.json 每条字段：
  id, text, category  必填
  voice               可选，覆盖默认 edge-tts 音色
  rate                可选，edge-tts 变速，如 "+40%" / "-20%"
  noise_snr_db        可选，混入近似该 SNR 的白噪声（语音 rms 按 0.5 估算）

已有三件套（.pcm/.ogg/.txt）且未加 --force 的语料跳过，避免重复打 edge-tts。

用法：
  python gen_corpus.py [--voice zh-CN-XiaoxiaoNeural] [--ffmpeg PATH] [--force]

退出码 0 = 全部生成成功，1 = 有失败。
"""
import argparse
import asyncio
import json
import math
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


def noise_amplitude(snr_db: float) -> float:
    """把目标 SNR(dB) 折算成白噪声 amplitude（语音 rms 近似 0.5，仅粗略压力测试用）。"""
    return round(0.5 * math.pow(10.0, -snr_db / 20.0), 4)


def mix_noise(ffmpeg: str, src: Path, dst: Path, snr_db: float) -> None:
    """ffmpeg 混入白噪声：anoisesrc + amix，时长取语音长度。"""
    amp = noise_amplitude(snr_db)
    subprocess.run(
        [ffmpeg, "-y", "-i", str(src),
         "-f", "lavfi", "-i", "anoisesrc=color=white:amplitude=" + str(amp),
         "-filter_complex", "[0:a][1:a]amix=inputs=2:duration=first:normalize=0[a]",
         "-map", "[a]", str(dst)],
        check=True, capture_output=True,
    )


async def gen_one(item: dict, default_voice: str, ffmpeg: str, tmpdir: Path,
                  force: bool) -> str:
    """生成单条语料的三件套产物。返回 'ok' 或 'skip'。失败抛异常。"""
    cid = item["id"]
    text = item["text"]
    voice = item.get("voice") or default_voice
    rate = item.get("rate")
    snr_db = item.get("noise_snr_db")

    pcm = CORPUS_DIR / f"{cid}.pcm"
    ogg = CORPUS_DIR / f"{cid}.ogg"
    txt = CORPUS_DIR / f"{cid}.txt"
    if not force and pcm.exists() and ogg.exists() and txt.exists():
        return "skip"

    mp3 = tmpdir / f"{cid}.mp3"
    kwargs = {}
    if rate:
        kwargs["rate"] = rate

    # 1. edge-tts -> mp3
    communicate = edge_tts.Communicate(text, voice, **kwargs)
    await communicate.save(str(mp3))
    if not mp3.exists() or mp3.stat().st_size == 0:
        raise RuntimeError("edge-tts 生成空 mp3")

    src = mp3
    # 可选：混白噪声
    if snr_db is not None:
        noisy = tmpdir / f"{cid}_noise.mp3"
        mix_noise(ffmpeg, mp3, noisy, float(snr_db))
        src = noisy

    # 2. -> raw PCM 16kHz mono s16le
    subprocess.run(
        [ffmpeg, "-y", "-i", str(src), "-ar", "16000", "-ac", "1", "-f", "s16le", str(pcm)],
        check=True, capture_output=True,
    )
    if not pcm.exists() or pcm.stat().st_size < 16000:
        raise RuntimeError(f"pcm 生成异常 size={pcm.stat().st_size if pcm.exists() else 0}")

    # 3. -> Ogg Opus 16kHz mono 32kbps voip（对齐固件编码场景）
    subprocess.run(
        [ffmpeg, "-y", "-i", str(src), "-ar", "16000", "-ac", "1",
         "-c:a", "libopus", "-b:a", "32k", "-application", "voip", str(ogg)],
        check=True, capture_output=True,
    )
    if not ogg.exists() or ogg.stat().st_size == 0:
        raise RuntimeError("ogg 生成异常")

    # 4. 写预期文本
    txt.write_text(text, encoding="utf-8")
    return "ok"


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
    ok = skip = 0
    failures: list[str] = []
    with tempfile.TemporaryDirectory() as td:
        tmpdir = Path(td)
        for item in manifest:
            try:
                result = await gen_one(item, args.voice, ffmpeg, tmpdir, args.force)
                if result == "skip":
                    print(f"  SKIP {item['id']} (exists)")
                    skip += 1
                else:
                    print(f"  OK   {item['id']}")
                    ok += 1
            except Exception as e:  # noqa: BLE001 - 逐条收集失败不中断整体
                msg = f"{item['id']}: {e}"
                print(f"  FAIL {msg}", file=sys.stderr)
                failures.append(msg)

    print(f"done: ok={ok} skip={skip} fail={len(failures)}")
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
    ap.add_argument("--force", action="store_true", help="重新生成已有语料")
    args = ap.parse_args()
    return asyncio.run(main_async(args))


if __name__ == "__main__":
    sys.exit(main())
