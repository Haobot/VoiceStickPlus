"""热词两个入口的确认小窗（Toplevel，仅在 tk 主线程创建）。

- HotwordDialog：框选添加确认（词条预填可改 + 读音变体逗号分隔）
- ConfirmRecentDialog：最近口述三段对照 + 纠正对勾选入库

只做契约与最小布局；视觉打磨与真机操作验收另见 scripts/。
"""
from __future__ import annotations

import tkinter as tk

from p1.orchestration.pipeline import PipelineResult

DLG_BG = "#1e1e2e"
DLG_FG = "#cdd6f4"
DLG_FONT = ("Microsoft YaHei UI", 11)


def _parse_prons(text: str) -> list[str]:
    """逗号/顿号分隔的读音变体输入 → 去空列表。"""
    return [p.strip() for p in text.replace("，", ",").replace("、", ",").split(",")
            if p.strip()]


def _ensure_modal(win) -> None:
    """先映射再 grab——对未 viewable 的窗口 grab_set 会抛 TclError
    （该异常曾杀死 overlay 轮询链，UI 全瘫）。

    弹窗进程往往不在前台（用户焦点在目标应用），必须置顶 + focus_force
    抢键盘焦点，否则确认键会打进目标应用。
    """
    win.update_idletasks()
    win.deiconify()
    win.attributes("-topmost", True)
    win.lift()
    win.focus_force()
    try:
        win.grab_set()
    except tk.TclError:  # 极端竞态下失去模态，可接受
        pass


class HotwordDialog:
    def __init__(self, root, initial_text: str, on_confirm):
        self._on_confirm = on_confirm
        self._win = tk.Toplevel(root)
        self._win.title("添加热词")
        self._win.configure(bg=DLG_BG, padx=16, pady=12)
        self._win.transient(root)
        self._win.grab_set()  # 模态

        tk.Label(self._win, text="词条（入库的规范写法）", bg=DLG_BG, fg=DLG_FG,
                 font=DLG_FONT).pack(anchor="w")
        self._surface = tk.Entry(self._win, font=DLG_FONT, width=36)
        self._surface.insert(0, initial_text)
        self._surface.pack(fill="x", pady=(2, 8))

        tk.Label(self._win, text="读音变体（逗号分隔，可留空；错读形式将来自动纠正）",
                 bg=DLG_BG, fg=DLG_FG, font=DLG_FONT).pack(anchor="w")
        self._pron = tk.Entry(self._win, font=DLG_FONT, width=36)
        self._pron.pack(fill="x", pady=(2, 10))

        row = tk.Frame(self._win, bg=DLG_BG)
        row.pack(fill="x")
        tk.Button(row, text="取消", command=self._win.destroy,
                  font=DLG_FONT).pack(side="right", padx=(8, 0))
        tk.Button(row, text="添加", command=self.confirm,
                  font=DLG_FONT).pack(side="right")
        self._surface.focus_set()
        self._surface.selection_range(0, "end")
        self._win.bind("<Return>", lambda e: self.confirm())
        _ensure_modal(self._win)

    # ---- 测试与内部共用的取值/设值 ----

    def surface_value(self) -> str:
        return self._surface.get().strip()

    def set_surface_value(self, text: str) -> None:
        self._surface.delete(0, "end")
        self._surface.insert(0, text)

    def set_pron_value(self, text: str) -> None:
        self._pron.delete(0, "end")
        self._pron.insert(0, text)

    def confirm(self) -> None:
        surface = self.surface_value()
        if not surface:
            return  # 空词条不入库不关窗
        prons = _parse_prons(self._pron.get())
        self._on_confirm(surface, prons)
        self._win.destroy()


class ConfirmRecentDialog:
    def __init__(self, root, result: PipelineResult, on_save):
        self._on_save = on_save
        self._result = result
        self._win = tk.Toplevel(root)
        self._win.title("确认最近口述")
        self._win.configure(bg=DLG_BG, padx=16, pady=12)
        self._win.transient(root)
        self._win.grab_set()

        for label, text in (("识别原文", result.raw_text),
                            ("热词纠正后", result.corrected_text or result.raw_text),
                            ("最终注入", result.final_text)):
            tk.Label(self._win, text=label, bg=DLG_BG, fg="#9399b2",
                     font=("Microsoft YaHei UI", 9)).pack(anchor="w")
            tk.Label(self._win, text=text or "（空）", bg=DLG_BG, fg=DLG_FG,
                     font=DLG_FONT, wraplength=360, justify="left",
                     anchor="w").pack(anchor="w", pady=(0, 6))

        self._vars: list[tk.BooleanVar] = []
        if result.corrections:
            tk.Label(self._win, text="记入热词库（错读形式将作为读音变体）",
                     bg=DLG_BG, fg="#9399b2",
                     font=("Microsoft YaHei UI", 9)).pack(anchor="w", pady=(6, 2))
            for event in result.corrections:
                var = tk.BooleanVar(value=True)
                self._vars.append(var)
                tk.Checkbutton(
                    self._win, variable=var, bg=DLG_BG, fg=DLG_FG, font=DLG_FONT,
                    text=f"{event.wrong} → {event.right}", anchor="w",
                    activebackground=DLG_BG, highlightthickness=0,
                ).pack(anchor="w")

        row = tk.Frame(self._win, bg=DLG_BG)
        row.pack(fill="x", pady=(10, 0))
        tk.Button(row, text="关闭", command=self._win.destroy,
                  font=DLG_FONT).pack(side="right", padx=(8, 0))
        tk.Button(row, text="保存勾选", command=self.save,
                  font=DLG_FONT).pack(side="right")
        self._win.bind("<Return>", lambda e: self.save())
        _ensure_modal(self._win)

    # ---- 测试与内部共用 ----

    def uncheck(self, index: int) -> None:
        self._vars[index].set(False)

    def save(self) -> None:
        pairs = [(event.wrong, event.right)
                 for event, var in zip(self._result.corrections, self._vars)
                 if var.get()]
        self._on_save(pairs)
        self._win.destroy()
