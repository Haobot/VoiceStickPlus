"""输出适配器：文本注入当前焦点窗口（路线图 §五 输出适配器 - 剪贴板通道）。

策略：剪贴板 + Ctrl+V 模拟（keyboard.write 对中文逐键不稳定，业界通行剪贴板回填）。
代价：覆盖用户剪贴板内容——P1 接受，P2 再做恢复。
"""
from __future__ import annotations

import time


class ClipboardInjector:
    """剪贴板写入 + 粘贴键模拟；send_paste=False 供测试只验剪贴板通道。"""

    def inject(self, text: str, send_paste: bool = True) -> float:
        """注入文本，返回端到端耗时秒（回填延迟验收口径的注入段）。"""
        if not text:
            raise ValueError("空文本不可注入")
        started = time.perf_counter()
        import pyperclip
        pyperclip.copy(text)
        if send_paste:
            import keyboard
            keyboard.send("ctrl+v")
        return time.perf_counter() - started
