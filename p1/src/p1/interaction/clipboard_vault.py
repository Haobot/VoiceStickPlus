"""剪贴板完整格式快照/恢复（纯 win32 ctypes，零新依赖）。

P1 两条路径（文本注入 / 框选读取）都借道剪贴板，用完必须还原用户原内容——
pyperclip 只会文本，图片/文件列表/HTML 会被永久覆盖。本模块枚举全部格式
逐项字节快照，恢复时 GlobalAlloc 新块写回。

跳过句柄类格式（非 HGLOBAL，GlobalLock 无意义）：CF_BITMAP/CF_METAFILEPICT/
CF_PALETTE/CF_ENHMETAFILE——位图场景应用几乎都会同时提供 CF_DIB/CF_DIBV5
内存版；metafile 丢失记入 README 已知限制。
"""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import time
from dataclasses import dataclass, field

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32

# 句柄类格式：数据不是 HGLOBAL，无法字节快照
_HANDLE_FORMATS = {2, 3, 9, 14}

_STANDARD_NAMES = {
    1: "CF_TEXT", 2: "CF_BITMAP", 3: "CF_METAFILEPICT", 4: "CF_SYLK",
    5: "CF_DIF", 6: "CF_TIFF", 7: "CF_OEMTEXT", 8: "CF_DIB",
    13: "CF_UNICODETEXT", 15: "CF_HDROP", 17: "CF_DIBV5",
}

LRESULT = ctypes.c_ssize_t
WNDPROC = ctypes.WINFUNCTYPE(LRESULT, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)
_VAULT_WND_CLASS = "VoiceStickP1VaultWnd"


@WNDPROC
def _vault_wndproc(hwnd, msg, wparam, lparam):
    return user32.DefWindowProcW(hwnd, msg, wparam, lparam)


class _WNDCLASSW(ctypes.Structure):
    _fields_ = [
        ("style", wt.UINT), ("lpfnWndProc", WNDPROC),
        ("cbClsExtra", ctypes.c_int), ("cbWndExtra", ctypes.c_int),
        ("hInstance", wt.HINSTANCE), ("hIcon", wt.HICON),
        ("hCursor", ctypes.c_void_p), ("hbrBackground", ctypes.c_void_p),
        ("lpszMenuName", wt.LPCWSTR), ("lpszClassName", wt.LPCWSTR),
    ]


def _setup_prototypes() -> None:
    # win64 不设原型 = 句柄/指针按 int 截断（本会话已三次实证必崩）
    user32.OpenClipboard.argtypes = (wt.HWND,)
    user32.OpenClipboard.restype = wt.BOOL
    user32.CloseClipboard.argtypes = ()
    user32.CloseClipboard.restype = wt.BOOL
    user32.EmptyClipboard.argtypes = ()
    user32.EmptyClipboard.restype = wt.BOOL
    user32.EnumClipboardFormats.argtypes = (wt.UINT,)
    user32.EnumClipboardFormats.restype = wt.UINT
    user32.GetClipboardData.argtypes = (wt.UINT,)
    user32.GetClipboardData.restype = ctypes.c_void_p
    user32.SetClipboardData.argtypes = (wt.UINT, ctypes.c_void_p)
    user32.SetClipboardData.restype = ctypes.c_void_p
    user32.IsClipboardFormatAvailable.argtypes = (wt.UINT,)
    user32.IsClipboardFormatAvailable.restype = wt.BOOL
    user32.GetClipboardFormatNameW.argtypes = (wt.UINT, wt.LPWSTR, ctypes.c_int)
    user32.GetClipboardFormatNameW.restype = ctypes.c_int
    user32.RegisterClassW.argtypes = (ctypes.c_void_p,)
    user32.RegisterClassW.restype = ctypes.c_ushort
    user32.CreateWindowExW.argtypes = (
        wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD,
        ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
        wt.HWND, wt.HMENU, wt.HINSTANCE, wt.LPVOID)
    user32.CreateWindowExW.restype = wt.HWND
    user32.DefWindowProcW.argtypes = (wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)
    user32.DefWindowProcW.restype = LRESULT
    user32.DestroyWindow.argtypes = (wt.HWND,)
    kernel32.GlobalAlloc.argtypes = (wt.UINT, ctypes.c_size_t)
    kernel32.GlobalAlloc.restype = ctypes.c_void_p
    kernel32.GlobalLock.argtypes = (ctypes.c_void_p,)
    kernel32.GlobalLock.restype = ctypes.c_void_p
    kernel32.GlobalUnlock.argtypes = (ctypes.c_void_p,)
    kernel32.GlobalUnlock.restype = wt.BOOL
    kernel32.GlobalSize.argtypes = (ctypes.c_void_p,)
    kernel32.GlobalSize.restype = ctypes.c_size_t
    kernel32.GlobalFree.argtypes = (ctypes.c_void_p,)
    kernel32.GlobalFree.restype = ctypes.c_void_p
    kernel32.GetModuleHandleW.argtypes = (wt.LPCWSTR,)
    kernel32.GetModuleHandleW.restype = wt.HINSTANCE


_setup_prototypes()


def _create_owner_window():
    """message-only 窗口做剪贴板 owner。

    本机实证（Win11 26200）：OpenClipboard(NULL) 写入的数据在 CloseClipboard
    时被释放——available 仍真但 GetClipboardData 空句柄；带 owner 窗口则保留。
    """
    hinst = kernel32.GetModuleHandleW(None)
    wc = _WNDCLASSW()
    wc.lpfnWndProc = _vault_wndproc
    wc.lpszClassName = _VAULT_WND_CLASS
    wc.hInstance = hinst
    user32.RegisterClassW(ctypes.byref(wc))   # 已存在（1410）= 复用
    return user32.CreateWindowExW(
        0, _VAULT_WND_CLASS, "vault", 0,
        0, 0, 0, 0, -3, None, hinst, None)    # -3 = HWND_MESSAGE


@dataclass
class ClipboardSnapshot:
    entries: list = field(default_factory=list)   # [(fmt, name, bytes)]

    def format_names(self) -> list[str]:
        return [name for _, name, _ in self.entries]

    def format_id(self, name: str) -> int | None:
        for fmt, fmt_name, _ in self.entries:
            if fmt_name == name:
                return fmt
        return None


class ClipboardVault:
    """OpenClipboard 带重试（并发占用是本机反复出现的现实）。"""

    def __init__(self, open_retries: int = 8, retry_delay: float = 0.025):
        self._open_retries = open_retries
        self._retry_delay = retry_delay
        self._owner_hwnd = _create_owner_window()

    def save(self) -> ClipboardSnapshot:
        """完整格式快照。打开失败抛 RuntimeError——空快照只代表真空剪贴板，
        混入"打不开"会让 restore 误清用户剪贴板。"""
        entries: list = []
        if not self._open():
            raise RuntimeError("剪贴板持续被占用，无法快照")
        try:
            fmt = 0
            while True:
                fmt = user32.EnumClipboardFormats(fmt)
                if fmt == 0:
                    break
                if fmt in _HANDLE_FORMATS:
                    continue
                handle = user32.GetClipboardData(fmt)
                if not handle:
                    continue
                size = kernel32.GlobalSize(handle)
                if size == 0:
                    continue
                ptr = kernel32.GlobalLock(handle)
                if not ptr:
                    continue
                try:
                    data = ctypes.string_at(ptr, size)
                finally:
                    kernel32.GlobalUnlock(handle)
                entries.append((fmt, self._format_name(fmt), data))
        finally:
            user32.CloseClipboard()
        return ClipboardSnapshot(entries)

    def restore(self, snapshot: ClipboardSnapshot) -> bool:
        if not self._open():
            return False
        try:
            user32.EmptyClipboard()
            for fmt, _name, data in snapshot.entries:
                handle = kernel32.GlobalAlloc(0x0002, len(data))   # GMEM_MOVEABLE
                if not handle:
                    continue
                ptr = kernel32.GlobalLock(handle)
                ctypes.memmove(ptr, data, len(data))
                kernel32.GlobalUnlock(handle)
                if not user32.SetClipboardData(fmt, handle):
                    kernel32.GlobalFree(handle)   # 写回失败自己释放，尽力恢复下一格式
        finally:
            user32.CloseClipboard()
        return True

    def _open(self) -> bool:
        for _ in range(self._open_retries):
            if user32.OpenClipboard(self._owner_hwnd):
                return True
            time.sleep(self._retry_delay)
        return False

    @staticmethod
    def _format_name(fmt: int) -> str:
        if fmt in _STANDARD_NAMES:
            return _STANDARD_NAMES[fmt]
        buf = ctypes.create_unicode_buffer(128)
        if user32.GetClipboardFormatNameW(fmt, buf, 128):
            return buf.value
        return f"CF_{fmt}"
