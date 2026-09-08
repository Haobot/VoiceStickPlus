#!/usr/bin/env python3
"""框选添加入口真机一体化验收：前台校验 → 注入 → 键盘确认 → 现场取证。"""
import ctypes
import subprocess
import sys
import time
from ctypes import wintypes
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "src"))
import pyperclip  # noqa: E402

user32 = ctypes.windll.user32
NOTEPAD_PID = 26112
APP_PID = 37376


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD),
                ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.c_void_p)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wintypes.DWORD), ("ki", KEYBDINPUT), ("pad", ctypes.c_ubyte * 8)]


def tap_key(vk: int, interval: float = 0.1) -> None:
    for flags in (0, 2):  # down, up
        inp = INPUT(type=1)
        inp.ki = KEYBDINPUT(vk, 0, flags, 0, None)
        user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))
        time.sleep(0.05)
    time.sleep(interval)


def send_scan_combo(mods, main, main_only_tap=True):
    def scan_key(scan, up=False):
        flags = 8 | (2 if up else 0)
        inp = INPUT(type=1)
        inp.ki = KEYBDINPUT(0, scan, flags, 0, None)
        user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))
    for m in mods:
        scan_key(m)
    scan_key(main)
    time.sleep(0.06)
    scan_key(main, up=True)
    for m in reversed(mods):
        scan_key(m, up=True)


def ctrl_a():
    """Ctrl+A（scan 注入：ctrl=0x1D，a=0x1E）。"""
    for scan, up in ((0x1D, False), (0x1E, False), (0x1E, True), (0x1D, True)):
        flags = 8 | (2 if up else 0)
        inp = INPUT(type=1)
        inp.ki = KEYBDINPUT(0, scan, flags, 0, None)
        user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))
        time.sleep(0.05)


def ctrl_v():
    """Ctrl+V（scan 注入：ctrl=0x1D，v=0x2F）。"""
    for scan, up in ((0x1D, False), (0x2F, False), (0x2F, True), (0x1D, True)):
        flags = 8 | (2 if up else 0)
        inp = INPUT(type=1)
        inp.ki = KEYBDINPUT(0, scan, flags, 0, None)
        user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))
        time.sleep(0.05)


def rewrite_notepad(word: str) -> None:
    """全选替换记事本内容为固定单词——消除环境人工改动干扰。"""
    ctrl_a()
    pyperclip.copy(word)
    time.sleep(0.1)
    ctrl_v()
    time.sleep(0.3)


def foreground_pid() -> int:
    hwnd = user32.GetForegroundWindow()
    pid = wintypes.DWORD()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    return pid.value


def find_window(pid: int, title_part: str):
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(hwnd, _):
        p = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid:
            length = user32.GetWindowTextLengthW(hwnd)
            buf = ctypes.create_unicode_buffer(length + 1)
            user32.GetWindowTextW(hwnd, buf, length + 1)
            if title_part in buf.value:
                found.append(buf.value)
        return True

    user32.EnumWindows(cb, 0)
    return found


def force_foreground(pid: int) -> bool:
    """AppActivate 的强力替补：恢复最小化 + ALT 解锁 + SetForegroundWindow。"""
    hwnd = None

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(h, _):
        nonlocal hwnd
        p = wintypes.DWORD()
        user32.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid and user32.IsWindowVisible(h):
            length = user32.GetWindowTextLengthW(h)
            buf = ctypes.create_unicode_buffer(length + 1)
            user32.GetWindowTextW(h, buf, length + 1)
            if buf.value:
                hwnd = h
                return False
        return True

    user32.EnumWindows(cb, 0)
    if not hwnd:
        return False
    if user32.IsIconic(hwnd):
        user32.ShowWindow(hwnd, 9)  # SW_RESTORE
        time.sleep(0.2)
    # ALT 按下再释放解除前台锁，SetForegroundWindow 才被允许
    user32.keybd_event(0x12, 0x38, 0, 0)
    user32.SetForegroundWindow(hwnd)
    user32.keybd_event(0x12, 0x38, 2, 0)
    time.sleep(0.3)
    return foreground_pid() == pid


def activate(pid: int, attempts: int = 3) -> bool:
    """环境常有其它窗口抢焦点，AppActivate 失败即切换强制路径。"""
    for _ in range(attempts):
        subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             f"$p = Get-Process -Id {pid}; "
             "[void][System.Reflection.Assembly]::LoadWithPartialName('Microsoft.VisualBasic');"
             f"[Microsoft.VisualBasic.Interaction]::AppActivate({pid}); exit 0"],
            capture_output=True)
        time.sleep(0.4)
        if foreground_pid() == pid:
            return True
        if force_foreground(pid):
            return True
    return False


def main() -> int:
    # ① 前台必须是记事本（Ctrl+C 复制的目标）
    if not activate(NOTEPAD_PID):
        print(f"[FAIL] 无法把记事本置于前台（当前前台 pid={foreground_pid()}）")
        return 1
    print("[1] 记事本前台 OK")
    # ② 重写记事本内容为固定单词并全选（环境真人多次改动内容，必须归一化起点）
    rewrite_notepad("Kubernetes")
    # 粘贴/激活操作可能让前台漂移，注入前最后一道闸
    if not activate(NOTEPAD_PID):
        print(f"[FAIL] 注入前记事本失焦（当前前台 pid={foreground_pid()}）")
        return 1
    ctrl_a()
    time.sleep(0.2)

    # ② 预置剪贴板标记（还原契约的基准）
    pyperclip.copy("CLIP_SAVED_MARKER")

    # ③ scan 注入 ctrl+alt+h（ctrl=0x1D alt=0x38 h=0x23）
    print(f"[取证] 注入时前台 pid={foreground_pid()}")
    send_scan_combo([0x1D, 0x38], 0x23)
    print(f"[取证] 注入后前台 pid={foreground_pid()}")
    time.sleep(0.55)  # worker 的 Ctrl+C 已发、还原未做（150ms 延时窗口）
    mid_clip = pyperclip.paste()
    time.sleep(1.2)
    print(f"[探针] Ctrl+C 实际复制到: {mid_clip!r}")

    # ④ 弹窗应出现在主程序进程
    wins = find_window(APP_PID, "添加热词")
    if not wins:
        print("[FAIL] 添加热词弹窗未出现")
        return 1
    print("[2] 弹窗出现 OK:", wins)

    # ⑤ 键盘确认：弹窗 Entry 聚焦，回车即确认（双触发已被防抖挡住）
    tap_key(0x0D)  # Enter
    time.sleep(1.5)

    # ⑥ 取证：弹窗消失 + 剪贴板还原
    wins_after = find_window(APP_PID, "添加热词")
    clip_now = pyperclip.paste()
    print(f"[3] 弹窗已关闭: {not wins_after}")
    print(f"[4] 剪贴板还原: {clip_now!r}")
    ok = (not wins_after) and clip_now == "CLIP_SAVED_MARKER"
    print("验收:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
