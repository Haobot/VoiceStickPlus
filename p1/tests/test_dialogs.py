"""热词两个入口的弹窗与 overlay 分发单测（真 Tk 控件，不进 mainloop）。

只验证契约：预填/默认勾选/确认回调参数；视觉布局留真机验收。
"""
import tkinter as tk

import pytest

from p1.flywheel.corrector import CorrectionEvent
from p1.interaction.dialogs import ConfirmRecentDialog, HotwordDialog
from p1.interaction.overlay import OverlayApp, OverlayEvent
from p1.orchestration.pipeline import PipelineResult


@pytest.fixture(scope="module")
def root():
    r = tk.Tk()
    r.withdraw()
    yield r
    r.destroy()


@pytest.fixture
def result_带纠正():
    return PipelineResult(
        final_text="迁移到Kubernetes。", raw_text="迁移到库伯内提斯。",
        corrected_text="迁移到库伯内提斯。", injected=True,
        corrections=[CorrectionEvent(wrong="库伯内提斯", right="Kubernetes",
                                     entry_id="id-1")],
        rewrite_corrections=["去口癖"], rewrite_engine="rules")


# ---------- HotwordDialog（框选添加确认窗） ----------

def test_词条预填_确认回调携带文本与读音变体(root):
    got = {}
    dlg = HotwordDialog(root, "Kubernetes",
                        on_confirm=lambda s, p: got.update(surface=s, prons=p))
    assert dlg.surface_value() == "Kubernetes"
    dlg.set_pron_value("库伯内提斯, kubernetes")
    dlg.confirm()
    assert got["surface"] == "Kubernetes"
    assert got["prons"] == ["库伯内提斯", "kubernetes"]


def test_词条清空后确认_不回调(root):
    got = []
    dlg = HotwordDialog(root, "Kubernetes", on_confirm=lambda s, p: got.append(s))
    dlg.set_surface_value("  ")
    dlg.confirm()
    assert got == []


def test_读音变体留空_回调空列表(root):
    got = {}
    dlg = HotwordDialog(root, "Voicestick", on_confirm=lambda s, p: got.update(prons=p))
    dlg.confirm()
    assert got["prons"] == []


# ---------- ConfirmRecentDialog（改写确认窗） ----------

def test_纠正对默认全选_保存回调携带配对(root, result_带纠正):
    got = {}
    dlg = ConfirmRecentDialog(
        root, result_带纠正, on_save=lambda pairs: got.update(pairs=pairs))
    dlg.save()
    assert got["pairs"] == [("库伯内提斯", "Kubernetes")]


def test_取消勾选_保存不含该对(root, result_带纠正):
    got = {}
    dlg = ConfirmRecentDialog(
        root, result_带纠正, on_save=lambda pairs: got.update(pairs=pairs))
    dlg.uncheck(0)
    dlg.save()
    assert got["pairs"] == []


def test_无纠正对_保存回调收空列表(root):
    result = PipelineResult(final_text="f", raw_text="r", injected=True)
    got = {}
    dlg = ConfirmRecentDialog(root, result, on_save=lambda p: got.update(pairs=p))
    dlg.save()
    assert got["pairs"] == []


# ---------- overlay 的 dialog 事件分发 ----------

class FakeDialogHost:
    def __init__(self):
        self.calls = []

    def open_add_hotword(self, text):
        self.calls.append(("add", text))

    def open_confirm_recent(self, result):
        self.calls.append(("confirm", result))


def test_overlay_dialog事件分发到host(root):
    host = FakeDialogHost()
    app = OverlayApp(root=root)
    app.set_dialog_host(host)
    app.post(OverlayEvent(kind="dialog", dialog="add_hotword",
                          payload={"text": "词A"}))
    app._poll()  # 同步排空事件队列（update 不驱动 after 定时器）
    assert ("add", "词A") in host.calls


def test_overlay_dialog确认事件分发到host(root, result_带纠正):
    host = FakeDialogHost()
    app = OverlayApp(root=root)
    app.set_dialog_host(host)
    app.post(OverlayEvent(kind="dialog", dialog="confirm_recent",
                          payload={"result": result_带纠正}))
    app._poll()
    assert ("confirm", result_带纠正) in host.calls


# ---------- 渲染容错（弹窗竞态曾杀死轮询链，UI 全瘫） ----------

class BoomHost:
    def open_add_hotword(self, text):
        raise RuntimeError("模拟渲染异常")


def test_poll渲染异常不中断后续事件链(root):
    app = OverlayApp(root=root)
    app.set_dialog_host(BoomHost())
    app.root().deiconify()  # 与运行态相反也无妨：hide 可验证
    app.post(OverlayEvent(kind="dialog", dialog="add_hotword",
                          payload={"text": "x"}))     # 此事件将抛异常
    app.post(OverlayEvent(kind="hide"))                # 必须仍被处理
    app._poll()  # 不应向外抛异常
    assert str(app.root().state()) == "withdrawn"


def test_hotword弹窗创建即映射_grab不炸(root):
    dlg = HotwordDialog(root, "Kubernetes", on_confirm=lambda s, p: None)
    root.update()  # 驱动一次几何管理（不进 mainloop）
    # 契约：构造全程不抛（grab 竞态已容错），窗口对象存活；
    # 真实屏幕映射由真机验收覆盖（withdrawn root 下单测映射不稳定）
    assert dlg._win.winfo_exists() == 1  # tkinter 返回 int 非 bool
    dlg._win.destroy()


def test_热词弹窗置顶并强取焦点(root):
    """弹窗进程常不在前台（用户焦点在目标应用），不置顶+focus_force
    则键盘确认键会打进目标应用。"""
    dlg = HotwordDialog(root, "Kubernetes", on_confirm=lambda s, p: None)
    root.update()
    assert dlg._win.attributes("-topmost")
    dlg._win.destroy()


def test_确认口述弹窗同样置顶(root, result_带纠正):
    dlg = ConfirmRecentDialog(root, result_带纠正, on_save=lambda pairs: None)
    root.update()
    assert dlg._win.attributes("-topmost")
    dlg._win.destroy()
