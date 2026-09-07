#!/usr/bin/env python3
"""第二迭代验收辅助：ctypes SendInput（VK 路径）注入两个入口组合键。

为什么不用 keyboard.send：keyboard 库 0.13.5 在 Windows 下合成修饰键组合
不可靠（'ctrl+alt+h' 零事件、单发 'alt' 零事件、'ctrl' 被错映射为 'c'）。
SendInput VK 路径与物理按键同链路（LL 钩子可见），已验证事件流完整。

用法:
    python inject_entries.py add       # ctrl+alt+h 框选添加
    python inject_entries.py confirm   # ctrl+alt+s 确认最近口述
"""
import ctypes
import sys
import time
from ctypes import wintypes
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "src"))


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD),
                ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.c_void_p)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wintypes.DWORD), ("ki", KEYBDINPUT),
                ("pad", ctypes.c_ubyte * 8)]


VK = {"ctrl": 0x11, "alt": 0x12, "h": 0x48, "s": 0x53}
# PS/2 set-1 make code（物理 scan code，add_hotkey 注册表按它索引）
SCAN = {"ctrl": 0x1D, "alt": 0x38, "h": 0x23, "s": 0x1F}
KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_SCANCODE = 0x0008


def _key(scan_code: int, up: bool = False) -> None:
    """KEYEVENTF_SCANCODE 注入（物理 scan 路径）。

    纯 VK 注入的事件 scan_code 为 -vk 负值，与 add_hotkey 注册表
    （物理 scan tuple，如 (29,35,56)）永远不匹配——真因见设计文档。
    """
    flags = KEYEVENTF_SCANCODE | (KEYEVENTF_KEYUP if up else 0)
    inp = INPUT(type=1)  # INPUT_KEYBOARD
    inp.ki = KEYBDINPUT(0, scan_code, flags, 0, None)
    ctypes.windll.user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))


def send_combo(modifiers: list[str], main: str) -> None:
    """按下所有修饰键 → 主键 down/up → 逆序释放修饰键。"""
    for mod in modifiers:
        _key(SCAN[mod])
    _key(SCAN[main])
    _key(SCAN[main], up=True)
    for mod in reversed(modifiers):
        _key(SCAN[mod], up=True)


if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else ""
    if mode == "add":
        send_combo(["ctrl", "alt"], "h")
        print("injected: ctrl+alt+h (SendInput)")
    elif mode == "confirm":
        send_combo(["ctrl", "alt"], "s")
        print("injected: ctrl+alt+s (SendInput)")
    else:
        print(f"unknown mode: {mode}")
        sys.exit(2)
    time.sleep(0.1)
