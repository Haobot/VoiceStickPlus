"""框选文本读取：剪贴板往返法（暂存 → Ctrl+C → 读 → 还原）。

已知取舍（设计文档 §2.1）：pyperclip 仅文本——非文本剪贴板（图片/文件）
无法还原，会被 Ctrl+C 覆盖；P2 用 win32 完整格式暂存。
"""
from __future__ import annotations

import ctypes
import time

MAX_SELECTION_CHARS = 64  # 热词是词条不是句子


def _clipboard_sequence() -> int:
    """系统剪贴板序列号：每次剪贴板内容更新递增，内容相同时也能判别。"""
    return ctypes.windll.user32.GetClipboardSequenceNumber()


class SelectionReader:
    # Ctrl+C 后剪贴板可能被并发占用（如调用方刚写入标记）导致复制静默失败，
    # 固定重试而非立即放弃
    COPY_RETRIES = 2

    def __init__(self, send_delay: float = 0.15):
        self._send_delay = send_delay

    def read_selection(self, send_copy: bool = True) -> str:
        """读取当前焦点应用的选中文本；无选择/超长/复制无响应抛 ValueError。

        send_copy=False 供单测：跳过 Ctrl+C 直接读剪贴板（预置内容）。
        """
        import pyperclip

        saved = pyperclip.paste()
        if send_copy:
            import keyboard
            seq_before = _clipboard_sequence()
            for _ in range(self.COPY_RETRIES + 1):
                keyboard.send("ctrl+c")
                time.sleep(self._send_delay)
                if _clipboard_sequence() != seq_before:
                    break
            else:
                # 序列号不动 = Ctrl+C 根本没改变剪贴板（复制失败）；
                # 若不拦下，saved（进入时的剪贴板旧值）会被误当选区文本
                raise ValueError("复制选区无响应，请确认目标窗口已选中文字")
        text = (pyperclip.paste() or "").strip()
        if text != saved:
            pyperclip.copy(saved)  # 还原进入时的剪贴板
        # 多行选择归一：词条是单行概念，换行折叠为空格
        text = " ".join(text.split())
        if not text:
            raise ValueError("未选中文本（或选区为空白），无法添加热词")
        if len(text) > MAX_SELECTION_CHARS:
            raise ValueError(f"选中文本过长（>{MAX_SELECTION_CHARS} 字符），热词应是词条而非句子")
        return text
