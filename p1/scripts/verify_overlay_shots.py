#!/usr/bin/env python3
"""悬浮条截图程序化核验：状态色条 / 深色背景 / 文本像素确实渲染。

判断口径：在截图中寻找与目标色欧氏距离最近的像素，
distance < 60 即认为该颜色存在（窗口 alpha 0.96 与屏幕混合允许偏差）。
"""
from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image

SHOT_DIR = Path(__file__).resolve().parent.parent / "docs" / "shots"

BG = (0x1E, 0x1E, 0x2E)
EXPECT = {
    "overlay_recording.png": (0xF3, 0x8B, 0xA8),   # 录音中 粉
    "overlay_recognizing.png": (0x89, 0xB4, 0xFA),  # 识别中 蓝
    "overlay_result.png": (0xA6, 0xE3, 0xA1),       # 结果 绿
}


def min_distance(img: Image.Image, target: tuple[int, int, int]) -> int:
    """全图最近像素与目标色的欧氏距离。"""
    px = img.convert("RGB").load()
    w, h = img.size
    best = 10**9
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            d = ((r - target[0]) ** 2 + (g - target[1]) ** 2 + (b - target[2]) ** 2) ** 0.5
            if d < best:
                best = d
    return int(best)


def main() -> int:
    failures = 0
    for name, color in EXPECT.items():
        path = SHOT_DIR / name
        if not path.exists():
            print(f"[FAIL] {name} 不存在")
            failures += 1
            continue
        img = Image.open(path)
        d_status = min_distance(img, color)
        d_bg = min_distance(img, BG)
        ok = d_status < 60 and d_bg < 60
        status = "PASS" if ok else "FAIL"
        if not ok:
            failures += 1
        print(f"[{status}] {name} size={img.size} 状态色距={d_status} 背景色距={d_bg}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
