"""打包测试语料 PCM 到 SPIFFS 镜像（L3 固件回放用）。

把 scripts/e2e_test/corpus/*.pcm 打包成 SPIFFS 镜像，供 esptool 烧到固件 storage 分区
（0x610000，大小 0x1f0000=1984KB）。固件 audio_pipeline_set_playback_file 读 /spiffs/<id>.pcm 回放。

spiffsgen.py 默认参数（page 256 / obj-name 32 / meta 4 / magic）与固件 sdkconfig 匹配，无需额外参数。

用法：python build_spiffs_image.py [--out spiffs_image.bin]
退出码 0 = 成功。
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

CORPUS_DIR = Path(__file__).resolve().parent / "corpus"
STORAGE_SIZE = 0x1f0000  # 1984KB，partitions_ota.csv 的 storage 分区大小


def find_spiffsgen() -> str | None:
    """定位 ESP-IDF spiffsgen.py：优先 IDF_PATH，其次已知安装路径。"""
    candidates = [
        Path(os.environ.get("IDF_PATH", "")) / "components" / "spiffs" / "spiffsgen.py",
        Path(r"C:\Espressif\frameworks\esp-idf-v5.5.1\components\spiffs\spiffsgen.py"),
    ]
    for c in candidates:
        if c.is_file():
            return str(c)
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description="Pack test corpus PCM into a SPIFFS image.")
    ap.add_argument("--out", default=str(CORPUS_DIR.parent / "spiffs_image.bin"),
                    help="output SPIFFS image path")
    args = ap.parse_args()

    sg = find_spiffsgen()
    if not sg:
        print("FAIL: spiffsgen.py not found (set IDF_PATH or check C:\\Espressif)", file=sys.stderr)
        return 1

    pcm_files = sorted(CORPUS_DIR.glob("*.pcm"))
    if not pcm_files:
        print(f"FAIL: no .pcm files in {CORPUS_DIR} (run gen_corpus.py first)", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        total = 0
        for pcm in pcm_files:
            shutil.copy2(pcm, tmp / pcm.name)
            total += pcm.stat().st_size
        print(f"packed {len(pcm_files)} pcm files, {total} bytes ({total / 1024:.0f} KiB)")

        # spiffsgen 默认参数与固件 sdkconfig 匹配（page 256/obj-name 32/meta 4/magic）
        r = subprocess.run(
            [sys.executable, sg, hex(STORAGE_SIZE), str(tmp), args.out],
            capture_output=True, text=True,
        )
        if r.returncode != 0:
            print(r.stdout)
            print(r.stderr, file=sys.stderr)
            print(f"FAIL: spiffsgen exit {r.returncode}", file=sys.stderr)
            return 1
        if r.stdout.strip():
            print(r.stdout.strip())

    out = Path(args.out)
    if not out.exists() or out.stat().st_size == 0:
        print(f"FAIL: output image not created {out}", file=sys.stderr)
        return 1
    print(f"OK: {out} size={out.stat().st_size} bytes ({out.stat().st_size / 1024:.0f} KiB)")
    if out.stat().st_size > STORAGE_SIZE:
        print(f"WARN: image {out.stat().st_size} > storage {STORAGE_SIZE}, will not fit!", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
