"""系统托盘常驻：TrayMenu 纯菜单模型 + TrayIcon 纯 ctypes win32 封装。

零新依赖（不引 pystray/PIL）：Shell_NotifyIconW + 私有窗口类收回调；
图标用 CreateIcon 单色点阵。托盘窗口与消息循环在独立线程，
菜单动作经回调闭包转交主线程执行（Tk destroy/keyboard.unhook 线程安全）。
"""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import logging
import threading
from dataclasses import dataclass

log = logging.getLogger("p1.tray")

WM_APP_TRAY = 0x8000 + 1      # 托盘回调消息

LRESULT = ctypes.c_ssize_t     # 本版 wintypes 缺 LRESULT/WNDCLASSW，手工补
WNDPROC = ctypes.WINFUNCTYPE(LRESULT, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)


class WNDCLASSW(ctypes.Structure):
    _fields_ = [
        ("style", wt.UINT), ("lpfnWndProc", WNDPROC),
        ("cbClsExtra", ctypes.c_int), ("cbWndExtra", ctypes.c_int),
        ("hInstance", wt.HINSTANCE), ("hIcon", wt.HICON),
        ("hCursor", ctypes.c_void_p), ("hbrBackground", ctypes.c_void_p),
        ("lpszMenuName", wt.LPCWSTR), ("lpszClassName", wt.LPCWSTR),
    ]


@dataclass
class TrayItem:
    label: str
    action: str = ""           # 空 = 无动作（状态行/占位）
    enabled: bool = True
    cmd_id: int = 0            # win32 菜单命令 id（add 时分配）
    checked: bool = False      # 勾选态（MF_CHECKED，开关项用）


class TrayMenu:
    """纯菜单模型：win32 命令 id ↔ 动作标识 的映射在这里，可全单测。

    分隔线不占命令 id（win32 MF_SEPARATOR 无 id），单独记插入位置。
    """

    def __init__(self):
        self._items: list[TrayItem] = []
        self._separators: set[int] = set()   # 分隔线插在哪个项索引之前

    def add(self, label: str, action: str = "", enabled: bool = True,
            checked: bool = False) -> TrayItem:
        item = TrayItem(label=label, action=action, enabled=enabled,
                        cmd_id=len(self._items) + 1, checked=checked)
        self._items.append(item)
        return item

    def add_separator(self) -> None:
        self._separators.add(len(self._items))

    def items(self) -> list[TrayItem]:
        """渲染序列：真实项之间按记录位置插入分隔线（label='-'）。"""
        out: list[TrayItem] = []
        for idx, item in enumerate(self._items):
            if idx in self._separators:
                out.append(TrayItem(label="-", action="", enabled=False))
            out.append(item)
        if len(self._items) in self._separators:   # 尾部分隔线
            out.append(TrayItem(label="-", action="", enabled=False))
        return out

    def labels(self) -> list[str]:
        return [item.label for item in self.items()]

    def find(self, action: str) -> TrayItem | None:
        for item in self._items:
            if item.action == action:
                return item
        return None

    def action_for(self, cmd_id: int) -> str | None:
        if 1 <= cmd_id <= len(self._items):
            return self._items[cmd_id - 1].action
        return None


# ---------- win32 薄封装（真机验收覆盖，单测只验证生命周期不炸） ----------

WNDCLASS_NAME = "VoiceStickP1TrayWnd"


class _NIF(ctypes.Structure):
    _fields_ = [
        ("cbSize", wt.UINT), ("hWnd", wt.HWND), ("uID", wt.UINT),
        ("uFlags", wt.UINT), ("uCallbackMessage", wt.UINT), ("hIcon", wt.HICON),
        ("szTip", wt.WCHAR * 128), ("dwState", wt.DWORD),
        ("dwStateMask", wt.DWORD), ("szInfo", wt.WCHAR * 256),
        ("uVersion", wt.UINT), ("szInfoTitle", wt.WCHAR * 64),
        ("dwInfoFlags", wt.DWORD),
    ]


# 16×16 单色 "V" 点阵：XOR 位（1=白）；AND 位全 0（不透明）
_V_ROWS = [
    "X..............X",
    "X..............X",
    "X.............X.",
    ".X............X.",
    ".X...........X..",
    ".X..........X...",
    "..X.........X...",
    "..X........X....",
    "..X.......X.....",
    "...X......X.....",
    "...X.....X......",
    "...X....X.......",
    "....X..X........",
    "....X.X.........",
    ".....XX.........",
    "................",
]


def _make_v_icon():
    """CreateIcon 单色 V 字图标；行序自下而上（win32 位图约定）。"""
    user32 = ctypes.windll.user32
    and_bits = (ctypes.c_ubyte * 32)()   # 16 行 × 2 字节，全 0 = 全不透明
    xor_bits = (ctypes.c_ubyte * 32)()
    for row, cells in enumerate(reversed(_V_ROWS)):   # 反转行序
        word = 0
        for col, ch in enumerate(cells):
            if ch == "X":
                word |= 1 << (15 - col)
        xor_bits[row * 2] = word & 0xFF
        xor_bits[row * 2 + 1] = word >> 8
    return user32.CreateIcon(0, 16, 16, 1, 1, and_bits, xor_bits)


# 窗口类进程级唯一：wndproc 必须模块级永活（实例闭包被 GC 后旧类再收消息
# = access violation），hwnd → 实例 的分发表在模块层维护。
_TRAY_BY_HWND: dict = {}


def _tray_wndproc(hwnd, msg, wparam, lparam):
    user32 = ctypes.windll.user32
    icon = _TRAY_BY_HWND.get(hwnd)
    if icon is not None:
        if msg == WM_APP_TRAY:
            if lparam in (0x0205, 0x0202):     # WM_RBUTTONUP / WM_LBUTTONUP
                icon._popup_menu(hwnd)
            return 0
        if msg == 0x0002:                       # WM_DESTROY：摘除分发表项
            _TRAY_BY_HWND.pop(hwnd, None)
    return user32.DefWindowProcW(hwnd, msg, wparam, lparam)


_MODULE_WNDPROC = WNDPROC(_tray_wndproc)   # 保活：模块级永不 GC


def _register_tray_class() -> bool:
    """注册进程级窗口类（重复注册 = 已存在，复用）。"""
    user32 = ctypes.windll.user32
    wc = WNDCLASSW()
    wc.lpfnWndProc = _MODULE_WNDPROC
    wc.lpszClassName = WNDCLASS_NAME
    wc.hInstance = ctypes.windll.kernel32.GetModuleHandleW(None)
    if not user32.RegisterClassW(ctypes.byref(wc)):
        return ctypes.windll.kernel32.GetLastError() in (0, 1410)
    return True


def _setup_prototypes() -> None:
    """win64 下不设 argtypes 的 ctypes 调用按 int 传参，
    指针截断 = access violation（曾杀死托盘线程）。一次性补全。"""
    user32 = ctypes.windll.user32
    shell32 = ctypes.windll.shell32
    user32.DefWindowProcW.argtypes = (wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)
    user32.DefWindowProcW.restype = LRESULT
    user32.RegisterClassW.argtypes = (ctypes.POINTER(WNDCLASSW),)
    user32.RegisterClassW.restype = ctypes.c_ushort
    user32.CreateWindowExW.argtypes = (
        wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD,
        ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
        wt.HWND, wt.HMENU, wt.HINSTANCE, wt.LPVOID)
    user32.CreateWindowExW.restype = wt.HWND
    user32.DestroyWindow.argtypes = (wt.HWND,)
    user32.GetMessageW.argtypes = (ctypes.POINTER(wt.MSG), wt.HWND,
                                   wt.UINT, wt.UINT)
    user32.GetMessageW.restype = ctypes.c_int
    user32.TranslateMessage.argtypes = (ctypes.POINTER(wt.MSG),)
    user32.DispatchMessageW.argtypes = (ctypes.POINTER(wt.MSG),)
    user32.PostMessageW.argtypes = (wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)
    user32.SetForegroundWindow.argtypes = (wt.HWND,)
    user32.CreatePopupMenu.restype = wt.HMENU
    user32.AppendMenuW.argtypes = (wt.HMENU, wt.UINT, ctypes.c_size_t, wt.LPCWSTR)
    user32.TrackPopupMenu.argtypes = (
        wt.HMENU, wt.UINT, ctypes.c_int, ctypes.c_int,
        ctypes.c_int, wt.HWND, ctypes.c_void_p)
    user32.TrackPopupMenu.restype = ctypes.c_int
    user32.GetCursorPos.argtypes = (ctypes.POINTER(wt.POINT),)
    user32.DestroyMenu.argtypes = (wt.HMENU,)
    user32.CreateIcon.argtypes = (wt.HINSTANCE, ctypes.c_int, ctypes.c_int,
                                  wt.UINT, wt.UINT, ctypes.c_void_p,
                                  ctypes.c_void_p)
    user32.CreateIcon.restype = wt.HICON
    user32.DestroyIcon.argtypes = (wt.HICON,)
    shell32.Shell_NotifyIconW.argtypes = (wt.DWORD, ctypes.c_void_p)
    shell32.Shell_NotifyIconW.restype = wt.BOOL
    kernel32 = ctypes.windll.kernel32
    # 64 位句柄不设 restype 会被截断成 32 位 int，塞进 WNDCLASSW 即崩
    kernel32.GetModuleHandleW.argtypes = (wt.LPCWSTR,)
    kernel32.GetModuleHandleW.restype = wt.HINSTANCE


class TrayIcon:
    """托盘图标：独立线程跑窗口消息循环；菜单动作回调转交调用方线程模型。

    on_action(action: str) 在托盘线程被调——实现方负责切回主线程
    （main.py 里经 overlay 事件队列转交）。
    """

    def __init__(self, menu_factory, on_action, tooltip: str = "VoiceStick P1"):
        self._menu_factory = menu_factory   # callable() -> TrayMenu（每次弹出重建，支持动态条数）
        self._on_action = on_action
        self._tooltip = tooltip
        self._thread: threading.Thread | None = None
        self._started = threading.Event()
        self._ok = False

    def start(self) -> bool:
        if self._thread is not None:
            return self._ok and self._started.is_set()
        self._thread = threading.Thread(target=self._run, daemon=True,
                                        name="p1-tray")
        self._thread.start()
        if not self._started.wait(timeout=3.0):
            return False
        return self._ok

    def stop(self) -> None:
        self._post_quit()

    def set_tooltip(self, tooltip: str) -> None:
        """动态更新悬停提示（如暂停态），任意线程可调（NIM_MODIFY 线程安全）。"""
        self._tooltip = tooltip
        nid = getattr(self, "_nid", None)
        if nid is not None:
            nid.szTip = tooltip
            ctypes.windll.shell32.Shell_NotifyIconW(
                1, ctypes.byref(nid))               # NIM_MODIFY

    # ---- 内部：全部运行在托盘线程 ----

    def _run(self) -> None:
        try:
            self._run_inner()
        except Exception:
            # 线程异常默认被吞、started 永不置位 = start() 假超时难排查
            log.exception("托盘线程异常退出")

    def _run_inner(self) -> None:
        _setup_prototypes()
        user32 = ctypes.windll.user32
        shell32 = ctypes.windll.shell32   # Shell_NotifyIconW 在 shell32，不在 user32
        self._user32 = user32
        self._shell32 = shell32
        self._hwnd = self._create_window()
        if not self._hwnd:
            log.error("托盘窗口创建失败")
            self._started.set()
            return
        self._icon = _make_v_icon()
        nid = _NIF()
        nid.cbSize = ctypes.sizeof(_NIF)
        nid.hWnd = self._hwnd
        nid.uID = 1
        nid.uFlags = 0x1 | 0x2 | 0x4        # NIF_MESSAGE | NIF_ICON | NIF_TIP
        nid.uCallbackMessage = WM_APP_TRAY
        nid.hIcon = self._icon
        nid.szTip = self._tooltip
        if not shell32.Shell_NotifyIconW(0, ctypes.byref(nid)):   # NIM_ADD
            log.error("托盘图标注册失败（NIM_ADD）")
            user32.DestroyWindow(self._hwnd)
            self._started.set()
            return
        self._nid = nid
        self._ok = True
        self._started.set()

        msg = wt.MSG()
        while user32.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))
        # 循环退出 = WM_QUIT：清理托盘与窗口
        shell32.Shell_NotifyIconW(2, ctypes.byref(self._nid))     # NIM_DELETE
        user32.DestroyWindow(self._hwnd)
        _TRAY_BY_HWND.pop(self._hwnd, None)
        if self._icon:
            user32.DestroyIcon(self._icon)

    def _create_window(self):
        if not _register_tray_class():
            return None
        hwnd = self._user32.CreateWindowExW(
            0, WNDCLASS_NAME, "VoiceStickP1Tray", 0,
            0, 0, 0, 0, None, None, None, None)
        if hwnd:
            _TRAY_BY_HWND[hwnd] = self    # 模块级 wndproc 经此分发
        return hwnd

    def _popup_menu(self, hwnd) -> None:
        user32 = self._user32
        menu = self._menu_factory()
        hmenu = user32.CreatePopupMenu()
        for item in menu.items():
            if item.label == "-":
                user32.AppendMenuW(hmenu, 0x800, 0, None)      # MF_SEPARATOR
            else:
                flags = 0x0                                    # MF_STRING
                if not item.enabled:
                    flags |= 0x1                               # MF_GRAYED
                if item.checked:
                    flags |= 0x8                               # MF_CHECKED
                user32.AppendMenuW(hmenu, flags, item.cmd_id, item.label)
        # 经典坑（KB135788）：不置前台则点击菜单外不消失
        user32.SetForegroundWindow(hwnd)
        pt = wt.POINT()
        user32.GetCursorPos(ctypes.byref(pt))
        cmd = user32.TrackPopupMenu(
            hmenu, 0x100, pt.x, pt.y, 0, hwnd, None)            # TPM_RETURNCMD，光标处弹出
        user32.PostMessageW(hwnd, 0x0001, 0, 0)                # WM_NULL 收尾
        user32.DestroyMenu(hmenu)
        if cmd:
            action = menu.action_for(cmd)
            if action:
                # TrackPopupMenu 已返回、不在窗口过程栈上，直接回调安全
                self._on_action(action)

    def _post_quit(self) -> None:
        if getattr(self, "_hwnd", None):
            self._user32.PostMessageW(self._hwnd, 0x0012, 0, 0)  # WM_QUIT
