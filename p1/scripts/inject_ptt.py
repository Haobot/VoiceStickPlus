#!/usr/bin/env python3
"""验收辅助：用 keyboard 库 SendInput 注入 PTT 键（与物理键同路径，跨进程可见）。

用法:
    python inject_ptt.py hold    # 按住 F8 2.5s 后松开（完整会话）
    python inject_ptt.py cancel  # 按住 F8 1.5s 后按 Esc 取消
"""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "src"))
import keyboard  # noqa: E402

KEY = sys.argv[1] if len(sys.argv) > 1 else "hold"

if KEY == "hold":
    keyboard.press("f8")
    time.sleep(2.5)
    keyboard.release("f8")
    print("injected: f8 hold 2.5s")
elif KEY == "cancel":
    keyboard.press("f8")
    time.sleep(1.5)
    keyboard.send("esc")
    time.sleep(0.1)
    keyboard.release("f8")
    print("injected: f8 1.5s then esc cancel")
else:
    print(f"unknown mode: {KEY}")
    sys.exit(2)
