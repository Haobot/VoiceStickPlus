"""输出适配器：文本注入当前焦点窗口（路线图 §五 输出适配器 - 剪贴板通道）。

策略：剪贴板 + Ctrl+V 模拟（keyboard.write 对中文逐键不稳定，业界通行剪贴板回填）。
第四迭代起注入完成即恢复用户原剪贴板（完整格式，含图片/HTML——见 clipboard_vault）。
"""
from __future__ import annotations

import time


class ClipboardInjector:
    """剪贴板写入 + 粘贴键模拟 + 用后恢复；send_paste=False 供测试只验剪贴板通道。

    restore_delay：粘贴是目标应用异步读剪贴板，立即恢复会粘贴出旧内容；
    延迟不计入返回的耗时（回填延迟口径 = 文字可落地时刻，恢复在其后）。
    """

    def __init__(self, restore_delay: float = 0.15):
        self._restore_delay = restore_delay

    def inject(self, text: str, send_paste: bool = True,
               restore_clipboard: bool = True) -> float:
        """注入文本，返回端到端耗时秒（回填延迟验收口径的注入段）。"""
        if not text:
            raise ValueError("空文本不可注入")
        from p1.interaction.clipboard_vault import ClipboardVault
        started = time.perf_counter()
        snapshot = None
        try:
            snapshot = ClipboardVault().save()
        except RuntimeError:
            pass   # 快照拿不到就不恢复（恢复不出错，但不能误清用户剪贴板）
        import pyperclip
        pyperclip.copy(text)
        elapsed = time.perf_counter() - started
        if send_paste:
            import keyboard
            keyboard.send("ctrl+v")
            elapsed = time.perf_counter() - started   # 粘贴已发出即文字将落地
        if restore_clipboard and snapshot is not None:
            if send_paste:
                time.sleep(self._restore_delay)
            ClipboardVault().restore(snapshot)
        return elapsed
