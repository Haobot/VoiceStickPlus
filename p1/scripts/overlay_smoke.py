#!/usr/bin/env python3
"""悬浮条视觉冒烟：定时驱动状态事件并截图（人工/自动化验收素材）。"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "src"))

from p1.interaction.overlay import OverlayApp, OverlayEvent

SHOT_DIR = Path(__file__).resolve().parent.parent / "docs" / "shots"


def main() -> int:
    SHOT_DIR.mkdir(parents=True, exist_ok=True)
    app = OverlayApp()
    root = app._root

    def shot(name: str) -> None:
        root.update_idletasks()
        root.update()
        x = root.winfo_rootx()
        y = root.winfo_rooty()
        from PIL import ImageGrab
        ImageGrab.grab(bbox=(x - 2, y - 2, x + 684, y + 76)).save(SHOT_DIR / name)

    def timeline():
        app.post(OverlayEvent(kind="recording", detail="松开出字"))
        root.after(600, lambda: shot("overlay_recording.png"))
        root.after(900, lambda: app.post(OverlayEvent(kind="recognizing", detail="录音 4.2s")))
        root.after(1500, lambda: shot("overlay_recognizing.png"))
        root.after(1800, lambda: app.post(OverlayEvent(
            kind="result", text="我们打算把整个服务迁移到Kubernetes集群上，这样扩容会方便很多。",
            corrections=(("kuubernetes", "Kubernetes"),), detail="0.28s")))
        root.after(2400, lambda: shot("overlay_result.png"))
        root.after(2800, root.destroy)

    root.after(300, timeline)
    root.mainloop()
    print(f"截图已保存: {SHOT_DIR}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
