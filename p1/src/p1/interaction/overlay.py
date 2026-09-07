"""悬浮条 UI：tkinter 无边框置顶状态条（录音中/识别中/结果反馈）。

线程模型：tkinter 只允许主线程操作，其他线程一律 post() 事件入队，
主循环 after(50ms) 轮询刷新（钩子线程/识别线程不直接碰控件）。
"""
from __future__ import annotations

import queue
import tkinter as tk
from dataclasses import dataclass

WIDTH, HEIGHT = 680, 72
BG = "#1e1e2e"
FG = "#cdd6f4"
COLOR_RECORDING = "#f38ba8"
COLOR_RECOGNIZING = "#89b4fa"
COLOR_RESULT = "#a6e3a1"
COLOR_ERROR = "#fab387"
RESULT_LINGER_MS = 4000  # 结果停留时长


@dataclass
class OverlayEvent:
    """一次 UI 状态更新（kind 决定渲染分支）。"""

    kind: str  # recording | recognizing | result | error | hide | level
    text: str = ""
    detail: str = ""
    level: float = 0.0
    corrections: tuple = ()
    timing: dict | None = None


class OverlayApp:
    """悬浮条应用；run() 进入 mainloop（主线程），其余线程用 post()。"""

    def __init__(self, position: tuple[int, int] | None = None):
        self.events: queue.Queue[OverlayEvent] = queue.Queue()
        self._root = tk.Tk()
        self._root.overrideredirect(True)   # 无边框
        self._root.attributes("-topmost", True)
        self._root.attributes("-alpha", 0.96)
        self._root.configure(bg=BG)
        screen_w = self._root.winfo_screenwidth()
        default_x = max(0, screen_w // 2 - WIDTH // 2)
        self._root.geometry(f"{WIDTH}x{HEIGHT}+{default_x}+48")
        if position:
            self._root.geometry(f"+{position[0]}+{position[1]}")

        self._bar = tk.Canvas(self._root, width=6, height=HEIGHT, bg=COLOR_RECORDING,
                              highlightthickness=0)
        self._bar.pack(side="left", fill="y")
        self._main = tk.Label(self._root, text="VoiceStick P1 就绪", fg=FG, bg=BG,
                              font=("Microsoft YaHei UI", 13), anchor="w", justify="left")
        self._main.pack(side="left", fill="both", expand=True, padx=(12, 8))
        self._detail = tk.Label(self._root, text="", fg="#9399b2", bg=BG,
                                font=("Consolas", 10), anchor="e", justify="right", width=18)
        self._detail.pack(side="right", fill="y", padx=(0, 10))
        self._root.withdraw()  # 空闲时隐藏，按住才出现

    # ---- 其他线程入口 ----

    def post(self, event: OverlayEvent) -> None:
        self.events.put(event)

    # ---- 主线程 ----

    def run(self) -> None:
        self._root.after(50, self._poll)
        self._root.mainloop()

    def _poll(self) -> None:
        try:
            while True:
                self._render(self.events.get_nowait())
        except queue.Empty:
            pass
        self._root.after(50, self._poll)

    def _render(self, event: OverlayEvent) -> None:
        if event.kind == "hide":
            self._root.withdraw()
            return
        if event.kind == "level":  # 录音电平刷新（不重排，只改条宽）
            self._root.after_idle(lambda: self._bar.configure(
                height=max(6, min(HEIGHT, int(event.level / 80)))))
            return
        self._root.deiconify()
        if event.kind == "recording":
            self._bar.configure(bg=COLOR_RECORDING)
            self._main.configure(text="● 录音中…", fg=FG)
        elif event.kind == "recognizing":
            self._bar.configure(bg=COLOR_RECOGNIZING)
            self._main.configure(text="◉ 识别中…", fg=FG)
        elif event.kind == "result":
            self._bar.configure(bg=COLOR_RESULT)
            fixed = "".join(f" [{w}→{r}]" for w, r in event.corrections)
            self._main.configure(text=event.text + fixed, fg=FG,
                                 wraplength=WIDTH - 120)
        elif event.kind == "error":
            self._bar.configure(bg=COLOR_ERROR)
            self._main.configure(text=f"⚠ {event.text}", fg=FG, wraplength=WIDTH - 120)
        self._detail.configure(text=event.detail)
        if event.kind == "result":
            self._root.after(RESULT_LINGER_MS, lambda: self._root.withdraw())
