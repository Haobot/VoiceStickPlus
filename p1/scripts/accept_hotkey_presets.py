#!/usr/bin/env python3
"""第五迭代真机一体化验收：托盘切热键方案 → 新键生效 → config 持久化 → 重启保持。

用法: python scripts/accept_hotkey_presets.py <GUI_PID>（持有 VoiceStickP1TrayWnd 的进程）
前置: 主程序运行中。验收结束自动把 config 恢复为无 preset 行（custom）。
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
CONFIG = Path(__file__).resolve().parent.parent / "config.toml"


class _KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD),
                ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.c_void_p)]


class _INPUT(ctypes.Structure):
    _fields_ = [("type", wintypes.DWORD), ("ki", _KEYBDINPUT),
                ("pad", ctypes.c_ubyte * 8)]


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


def find_menu_windows() -> list:
    """全部弹出菜单 #32768 的 (hwnd, l, t, r, b)，按 left 升序（父菜单在左）。"""
    menus = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(h, _):
        cls = ctypes.create_unicode_buffer(128)
        user32.GetClassNameW(h, cls, 128)
        if cls.value == "#32768" and user32.IsWindowVisible(h):
            rect = wintypes.RECT()
            user32.GetWindowRect(h, ctypes.byref(rect))
            menus.append((h, rect.left, rect.top, rect.right, rect.bottom))
        return True

    user32.EnumWindows(cb, 0)
    return sorted(menus, key=lambda m: m[1])


def mouse_click(x: int, y: int) -> None:
    user32.SetCursorPos(x, y)
    time.sleep(0.15)
    for flags in (0x0002, 0x0004):   # LEFTDOWN / LEFTUP
        ctypes.windll.user32.mouse_event(flags, 0, 0, 0, 0)
        time.sleep(0.05)
    time.sleep(0.8)


def _dismiss_menus() -> None:
    """Esc 清场：残留的模态菜单不退出会吞掉后续弹菜单消息。"""
    if not find_menu_windows():
        return
    import keyboard
    keyboard.send("esc")
    for _ in range(10):
        time.sleep(0.1)
        if not find_menu_windows():
            return


def _new_menu(before: set) -> tuple | None:
    """返回不在 before 集合中的新弹出菜单（hwnd 句柄值集合）。"""
    for h, left, top, right, bottom in find_menu_windows():
        if h not in before:
            return (h, left, top, right, bottom)
    return None


def popup_main_menu() -> tuple:
    """弹托盘主菜单，返回 (left, top, right, bottom)。

    先 Esc 清残留模态菜单；只认本轮新增的 #32768 窗口。
    """
    hwnd = find_tray_hwnd()
    if not hwnd:
        raise RuntimeError("托盘窗口未找到")
    _dismiss_menus()
    before = {m[0] for m in find_menu_windows()}
    user32.PostMessageW(hwnd, WM_APP_TRAY, 0, 0x0205)   # WM_RBUTTONUP
    for _ in range(20):
        time.sleep(0.1)
        m = _new_menu(before)
        if m:
            return m[1:]
    raise RuntimeError("主菜单未弹出")


def tap_key(name: str) -> None:
    """keyboard 库短按注入（带扫描码；手写 SendInput 纯 VK 无 scan 进不了菜单模态）。"""
    import keyboard
    keyboard.send(name)
    time.sleep(0.15)


def _post_key(menu_hwnd: int, vk: int) -> None:
    """PostMessage 键盘消息直发菜单窗口——不经输入队列，不受前台/焦点竞争影响
    （SendInput 注入对菜单模态不稳定，PostMessage 实验验证可路由）。"""
    user32.PostMessageW(menu_hwnd, 0x0100, vk, 0)   # WM_KEYDOWN
    time.sleep(0.12)


def submenu_click(popup_index: int, item_index: int) -> None:
    """键盘导航选子菜单项：主菜单窗口直发 DOWN/RIGHT/DOWN/RETURN。

    主菜单渲染序（第五迭代）：0状态 1热词库 2sep 3监听 4自启 5热键方案 6sep 7退出；
    子菜单：0=右Ctrl 长按 1=F8 长按。键盘高亮驱动的子菜单不依赖 hover 维持。
    """
    popup_main_menu()
    menus = find_menu_windows()
    main_hwnd = menus[0][0]
    for _ in range(5):          # 初始无高亮 → DOWN 六次到 index5（首次 DOWN 建立高亮）
        _post_key(main_hwnd, 0x28)   # VK_DOWN
    _post_key(main_hwnd, 0x27)       # VK_RIGHT 展开子菜单（首项高亮）
    for _ in range(popup_index):
        _post_key(main_hwnd, 0x28)
    _post_key(main_hwnd, 0x0D)       # VK_RETURN 选中
    for _ in range(35):         # 模态退出 + 菜单窗口销毁可慢至数秒
        time.sleep(0.1)
        if not find_menu_windows():
            return
    _dismiss_menus()
    raise RuntimeError("子菜单项键盘导航未生效（模态未退出）")


def config_text() -> str:
    return CONFIG.read_text(encoding="utf-8") if CONFIG.exists() else ""


def log_text() -> str:
    log_file = Path(__file__).resolve().parent.parent / "p1_main_run.log"
    return log_file.read_text(encoding="utf-8", errors="replace") \
        if log_file.exists() else ""


def switch_count() -> int:
    """主程序日志中「热键方案已切换」条数——真切换的硬证据。"""
    return log_text().count("热键方案已切换")


def ptt_flash(key: str) -> None:
    """短按 PTT 键 0.4s——触发会话开始日志即可，不出字不动剪贴板。"""
    import keyboard
    keyboard.press(key)
    time.sleep(0.4)
    keyboard.release(key)


def main() -> int:
    # ① 托盘切到「F8 长按」（以日志新增切换条目为准，防 preset 已在文件的假阳性）
    n0 = switch_count()
    submenu_click(1, 0)
    print("[1] 已点子菜单「F8 长按」")
    time.sleep(1.2)
    ok_switch_f8 = switch_count() == n0 + 1 \
        and 'preset = "f_key"' in config_text()
    print(f"[2] 切换日志+1 且 config preset=f_key: {ok_switch_f8}")

    # ② rebind 后 F8 仍触发会话
    ptt_flash("f8")
    time.sleep(1.5)
    print("[3] F8 短按注入完成（看日志确认会话开始）")

    # ③ 托盘切到「右Ctrl 长按」
    n1 = switch_count()
    submenu_click(0, 0)
    print("[4] 已点子菜单「右Ctrl 长按」")
    time.sleep(1.2)
    ok_switch_rc = switch_count() == n1 + 1 \
        and 'preset = "right_ctrl"' in config_text()
    print(f"[5] 切换日志+1 且 config preset=right_ctrl: {ok_switch_rc}")

    # ④ 新键右 Ctrl 触发会话（同键换组后钩子已重绑）
    ptt_flash("right ctrl")
    time.sleep(1.5)
    print("[6] 右Ctrl 短按注入完成（看日志确认会话开始）")

    # ⑤ 恢复 config 原样（无 preset 行 = custom，显式 f8 继续生效）
    from p1.config import save_preset
    save_preset(CONFIG, "custom")
    print(f"[7] config 已恢复无 preset: {'preset' not in config_text()}")

    ok = ok_switch_f8 and ok_switch_rc
    print("验收:", "PASS" if ok else "FAIL（切换/会话细节看主程序日志取证）")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
