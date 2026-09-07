"""ClipboardVault 单测：真剪贴板完整格式快照/恢复（自保存自还原现场）。

HTML Format / CF_DIB 等多格式场景是本迭代的存在理由——pyperclip 只会文本，
图片/HTML 被覆盖即丢失。
"""
import ctypes
import ctypes.wintypes as wt

import pytest

from p1.interaction.clipboard_vault import ClipboardVault

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32

CF_TEXT, CF_UNICODETEXT, CF_DIB = 1, 13, 8


def _set_bytes(fmt: int, data: bytes) -> None:
    _set_multi({fmt: data})


def _set_multi(formats: dict) -> None:
    """一次打开会话写多格式（Open 后必须先 Empty 再 Set；多格式须同会话）。

    预置写入撞上环境占用（系统剪贴板管理器周期扫描）时如实 SKIP——
    静默失败会让后续断言变成假失败。
    """
    if not user32.OpenClipboard(0):
        pytest.skip("剪贴板被环境占用，预置失败")
    try:
        user32.EmptyClipboard()
        for fmt, data in formats.items():
            _set_without_open(fmt, data)
    finally:
        user32.CloseClipboard()


def _set_without_open(fmt: int, data: bytes) -> None:
    handle = kernel32.GlobalAlloc(0x0002, len(data))   # GMEM_MOVEABLE
    ptr = kernel32.GlobalLock(handle)
    ctypes.memmove(ptr, data, len(data))
    kernel32.GlobalUnlock(handle)
    user32.SetClipboardData(fmt, handle)


def _get_bytes(fmt: int) -> bytes | None:
    # GetClipboardData 要求剪贴板处于打开状态（IsClipboardFormatAvailable 不要求）
    if not user32.IsClipboardFormatAvailable(fmt):
        return None
    if not user32.OpenClipboard(0):
        return None
    try:
        handle = user32.GetClipboardData(fmt)
        if not handle:
            return None
        size = kernel32.GlobalSize(handle)
        ptr = kernel32.GlobalLock(handle)
        data = ctypes.string_at(ptr, size)
        kernel32.GlobalUnlock(handle)
        return data
    finally:
        user32.CloseClipboard()


@pytest.fixture
def clipboard_guard():
    """保存在场全部格式 → 清空 → 测试 → 还原；环境持续占用剪贴板时如实 SKIP。"""
    vault = ClipboardVault()
    try:
        snapshot = vault.save()
    except RuntimeError as exc:
        pytest.skip(f"剪贴板被环境占用，无法测：{exc}")
    user32.OpenClipboard(0)
    user32.EmptyClipboard()
    user32.CloseClipboard()
    yield
    vault.restore(snapshot)


def test_save打开失败_抛RuntimeError不伪造空快照(monkeypatch):
    """打开失败若返回空快照，恢复时会误清用户剪贴板——必须抛错。"""
    vault = ClipboardVault()
    monkeypatch.setattr(vault, "_open", lambda: False)
    with pytest.raises(RuntimeError, match="占用"):
        vault.save()


def test_文本与HTML双格式_快照恢复后都在(clipboard_guard):
    html = b"Version:0.9\r\nStartHTML:00097\r\n<html><body>test</body></html>"
    html_fmt = user32.RegisterClipboardFormatW("HTML Format")
    assert html_fmt, "注册格式失败"
    _set_multi({
        CF_UNICODETEXT: "迁移到Kubernetes。".encode("utf-16-le") + b"\x00\x00",
        html_fmt: html,
    })
    vault = ClipboardVault()
    snap = vault.save()
    assert "HTML Format" in snap.format_names()
    # 覆盖成无关纯文本（模拟 P1 注入）
    import pyperclip
    pyperclip.copy("覆盖文本")
    # 恢复后两种格式都必须回来
    vault.restore(snap)
    got_text = _get_bytes(CF_UNICODETEXT)
    assert got_text is not None
    assert "迁移到Kubernetes。".encode("utf-16-le") in got_text
    assert _get_bytes(html_fmt) == html


def test_空剪贴板_快照恢复后仍空(clipboard_guard):
    user32.OpenClipboard(0)
    user32.EmptyClipboard()
    user32.CloseClipboard()
    vault = ClipboardVault()
    snap = vault.save()
    assert snap.format_names() == []
    import pyperclip
    pyperclip.copy("临时内容")
    vault.restore(snap)
    assert pyperclip.paste() is None or pyperclip.paste() == ""


def test_纯文本_字节级往返(clipboard_guard):
    payload = "剪贴板恢复测试".encode("utf-16-le") + b"\x00\x00"
    _set_bytes(CF_UNICODETEXT, payload)
    vault = ClipboardVault()
    snap = vault.save()
    pyperclip_copy = "别的"
    _set_bytes(CF_UNICODETEXT, pyperclip_copy.encode("utf-16-le") + b"\x00\x00")
    vault.restore(snap)
    assert _get_bytes(CF_UNICODETEXT) == payload


def test_CF_DIB位图字节_快照恢复(clipboard_guard):
    # 最小 1x1 32bpp DIB：BITMAPINFOHEADER(40B) + 1 像素
    import struct
    dib = struct.pack("<IiiHHIIiiII", 40, 1, 1, 1, 32, 0, 4, 0, 0, 0, 0) + b"\x11\x22\x33\x00"
    _set_bytes(CF_DIB, dib)
    vault = ClipboardVault()
    snap = vault.save()
    assert "CF_DIB" in snap.format_names() or 8 in [snap.format_id(n) for n in snap.format_names()]
    pyperclip_copy = "文本覆盖"
    _set_bytes(CF_UNICODETEXT, pyperclip_copy.encode("utf-16-le") + b"\x00\x00")
    vault.restore(snap)
    assert _get_bytes(CF_DIB) == dib


