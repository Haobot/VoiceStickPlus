"""语料产物验证脚本（M1.1 的「红灯」测试）。

遍历 corpus/corpus.json，断言每条语料的产物齐全且格式正确：
  - {id}.pcm : 16kHz / mono / s16le raw PCM，至少 0.5s（>= 16000 字节），字节数为偶数
  - {id}.ogg : Ogg Opus，前 4 字节为 'OggS' magic，首 1KB 内含 'OpusHead'
  - {id}.txt : UTF-8 文本，内容非空且与清单 text 一致

退出码 0 = 全部通过，1 = 有失败项。gen_corpus.py 产出产物前，本脚本必然失败（红灯）。
"""
import json
import sys
from pathlib import Path

CORPUS_DIR = Path(__file__).resolve().parent / "corpus"
MIN_PCM_BYTES = 16000  # 0.5s @ 16kHz/16bit/mono = 16000 字节（对齐固件 <0.5s 丢弃阈值）


def verify_one(item: dict) -> list[str]:
    """返回该条语料的失败原因列表，空列表表示通过。"""
    failures: list[str] = []
    cid = item["id"]

    pcm = CORPUS_DIR / f"{cid}.pcm"
    if not pcm.exists():
        failures.append(f"{cid}: 缺少 {pcm.name}")
    else:
        raw = pcm.read_bytes()
        if len(raw) < MIN_PCM_BYTES:
            failures.append(f"{cid}: pcm 过短 {len(raw)} 字节（需 >= {MIN_PCM_BYTES}）")
        if len(raw) % 2 != 0:
            failures.append(f"{cid}: pcm 字节数非偶数，非 s16le")

    ogg = CORPUS_DIR / f"{cid}.ogg"
    if not ogg.exists():
        failures.append(f"{cid}: 缺少 {ogg.name}")
    else:
        odata = ogg.read_bytes()
        if odata[:4] != b"OggS":
            failures.append(f"{cid}: ogg magic 非 OggS")
        if b"OpusHead" not in odata[:1024]:
            failures.append(f"{cid}: ogg 首 1KB 未找到 OpusHead")

    txt = CORPUS_DIR / f"{cid}.txt"
    if not txt.exists():
        failures.append(f"{cid}: 缺少 {txt.name}")
    else:
        content = txt.read_text(encoding="utf-8").strip()
        if not content:
            failures.append(f"{cid}: txt 为空")
        elif content != item["text"]:
            failures.append(f"{cid}: txt 与清单不一致：期望「{item['text']}」实际「{content}」")

    return failures


def main() -> int:
    manifest_path = CORPUS_DIR / "corpus.json"
    if not manifest_path.exists():
        print(f"FAIL: 语料清单不存在 {manifest_path}")
        return 1

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    all_failures: list[str] = []
    for item in manifest:
        all_failures.extend(verify_one(item))

    if all_failures:
        print(f"FAIL: {len(all_failures)} 项校验失败")
        for f in all_failures:
            print(f"  - {f}")
        return 1

    print(f"OK: {len(manifest)} 条语料产物全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
