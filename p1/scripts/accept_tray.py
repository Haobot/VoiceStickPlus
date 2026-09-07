#!/usr/bin/env python3
"""托盘第二批真机一体化验收：菜单开关导航 + 暂停时热键不响应 + 自启注册表取证。

用法: python scripts/accept_tray.py <GUI_PID>（持有 VoiceStickP1TrayWnd 窗口的 python 进程）
前置: 主程序运行中，config push_to_talk=f8。
"""
import ctypes
import subprocess
import sys
import time
from ctypes import wintypes
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "src"))

user32 = ctypes.windll.user32
WM_APP_TRAY = 0x8001
GUI_PID = int(sys.argv[1])


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD),
                ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.c_void_p)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wintypes.DWORD), ("ki", KEYBDINPUT), ("pad", ctypes.c_ubyte * 8)]


def tap(vk: int, interval: float = 0.12) -> None:
    for flags in (0, 2):
        inp = INPUT(type=1)
        inp.ki = KEYBDINPUT(vk, 0, flags, 0, None)
        user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))
        time.sleep(0.04)
    time.sleep(interval)


def find_tray_hwnd() -> int:
    hwnd = ctypes.c_void_p()

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(h, _):
        p = wintypes.DWORD()
        user32.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == GUI_PID:
            cls = ctypes.create_unicode_buffer(128)
            user32.GetClassNameW(h, cls, 128)
            if cls.value == "VoiceStickP1TrayWnd":
                hwnd.value = h
                return False
        return True

    user32.EnumWindows(cb, 0)
    return hwnd.value


def find_menu_window():
    """返回弹出菜单 (#32768) 的 (hwnd, left, top, right, bottom)。"""
    menus = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(h, _):
        cls = ctypes.create_unicode_buffer(128)
        user32.GetClassNameW(h, cls, 128)
        if cls.value == "#32768":
            rect = wintypes.RECT()
            user32.GetWindowRect(h, ctypes.byref(rect))
            menus.append((h, rect.left, rect.top, rect.right, rect.bottom))
        return True

    user32.EnumWindows(cb, 0)
    return menus[0] if menus else None


def menu_click(item_index: int) -> None:
    """弹托盘菜单 → 鼠标点击第 item_index 项（按渲染顺序含分隔线，0 起）。"""
    hwnd = find_tray_hwnd()
    if not hwnd:
        raise RuntimeError("托盘窗口未找到")
    user32.PostMessageW(hwnd, WM_APP_TRAY, 0, 0x0205)  # WM_RBUTTONUP
    for _ in range(20):      # 等 TrackPopupMenu 进入模态
        time.sleep(0.1)
        m = find_menu_window()
        if m:
            break
    if not m:
        raise RuntimeError("菜单未弹出")
    _, left, top, right, bottom = m
    height = bottom - top
    count = 7                 # 状态/热词库/sep/监听/自启/sep/退出
    row_h = height / count
    x = left + (right - left) // 2
    y = int(top + row_h * item_index + row_h / 2)
    ctypes.windll.user32.SetCursorPos(x, y)
    time.sleep(0.15)
    for flags in (0x0002, 0x0004):   # MOUSEEVENTF_LEFTDOWN / LEFTUP
        ctypes.windll.user32.mouse_event(flags, 0, 0, 0, 0)
        time.sleep(0.05)
    time.sleep(0.8)


def run_entry() -> str | None:
    out = subprocess.run(
        ["powershell", "-NoProfile", "-Command",
         "(Get-ItemProperty 'HKCU:\\Software\\Microsoft\\Windows\\CurrentVersion\\Run'"
         " -ErrorAction SilentlyContinue).VoiceStickP1"],
        capture_output=True, text=True)
    value = out.stdout.strip()
    return value or None


def inject_ptt_short() -> None:
    """短按 F8（0.4s）——足够触发会话开始日志（音频过短不出字，不打扰剪贴板）。"""
    import keyboard
    keyboard.press("f8")
    time.sleep(0.4)
    keyboard.release("f8")


def main() -> int:
    from p1.interaction import autostart
    ok = True

    # 菜单渲染顺序：0状态 1热词库 2sep 3监听热键 4开机自启 5sep 6退出
    # ① 暂停：点「监听热键」
    menu_click(3)
    print("[1] 已点「监听热键」（应暂停）")
    time.sleep(1.0)

    # ② 暂停中 F8 不应触发会话
    inject_ptt_short()
    time.sleep(1.5)
    print("[2] 暂停中注入 F8 完成（看日志确认无会话开始）")

    # ③ 恢复监听
    menu_click(3)
    print("[3] 已再点「监听热键」（应恢复）")
    time.sleep(1.0)

    # ④ 恢复后 F8 应触发会话（PTT 回归）
    inject_ptt_short()
    time.sleep(2.0)
    print("[4] 恢复后注入 F8 完成（看日志确认会话开始）")

    # ⑤ 开机自启用：点「开机自启」
    menu_click(4)
    time.sleep(1.0)
    value = run_entry()
    print(f"[5] 自启注册表: {value!r}")
    ok = ok and value is not None and "main.py" in value

    # ⑥ 关闭自启
    menu_click(4)
    time.sleep(1.0)
    value2 = run_entry()
    print(f"[6] 关闭后注册表: {value2!r}")
    ok = ok and value2 is None

    print("验收:", "PASS" if ok else "FAIL（菜单动作链看主程序日志取证）")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
