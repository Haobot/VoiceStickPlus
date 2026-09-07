"""VoiceController 单测：最近口述缓存 + 两个热词入口回调（状态机契约）。

依赖策略：overlay/session 用轻量 fake 记录事件（单测不进 tk mainloop）；
hotword_store 用真库（tmp_path）；selection reader 用 stub（Ctrl+C 路径真机验收）。
"""
import threading

import pytest

from p1.controller import VoiceController
from p1.flywheel.corrector import CorrectionEvent
from p1.orchestration.pipeline import PipelineResult


class FakeOverlay:
    def __init__(self):
        self.events = []

    def post(self, event):
        self.events.append(event)

    def root(self):
        return None  # 弹窗类已被 monkeypatch，root 不会被真正使用


class FakeSession:
    def start(self):
        pass

    def stop(self):
        import numpy as np
        return np.zeros(1600, dtype=np.int16)  # 0.1s < 最小时长 → None 路径


class FakePipeline:
    """process 返回 None（短音频），仅供控制器默认流不炸。"""

    def __init__(self, result=None):
        self._result = result

    def process(self, samples):
        return self._result


class StubSelection:
    def __init__(self, text=None, error=None):
        self._text = text
        self._error = error
        self.calls = 0

    def read_selection(self, send_copy=True):
        self.calls += 1
        if self._error:
            raise self._error
        return self._text


def make_result(corrections=()):
    return PipelineResult(
        final_text="我们打算迁移到Kubernetes。", raw_text="我们打算迁移到库伯内提斯。",
        injected=True,
        corrections=list(corrections),
        rewrite_corrections=["去口癖"], rewrite_engine="rules",
        timing={"total_seconds": 0.1})


def make_controller(store, selection, pipeline=None, overlay=None):
    return VoiceController(
        session=FakeSession(), pipeline=pipeline or FakePipeline(),
        overlay=overlay or FakeOverlay(),
        hotword_store=store, selection_reader=selection)


# ---------- 最近口述缓存 ----------

def test_口述成功后缓存最近结果(store):
    ctl = make_controller(store, StubSelection())
    ctl._process_worker_result(make_result())  # 直接喂 worker 的结果处理点
    assert ctl.last_result is not None
    assert ctl.last_result.final_text == "我们打算迁移到Kubernetes。"


def test_新口述覆盖旧缓存(store):
    ctl = make_controller(store, StubSelection())
    first = make_result()
    second = make_result()
    second.final_text = "第二句"
    ctl._process_worker_result(first)
    ctl._process_worker_result(second)
    assert ctl.last_result.final_text == "第二句"


# ---------- 框选添加入口 ----------

def test_框选添加_读取成功后发弹窗事件(store):
    overlay = FakeOverlay()
    ctl = make_controller(store, StubSelection(text="Voicestick"), overlay=overlay)
    ctl.on_add_selection()
    # 事件在后台线程 post，等待落定
    for _ in range(50):
        if overlay.events:
            break
        threading.Event().wait(0.02)
    assert overlay.events[-1].kind == "dialog"
    assert overlay.events[-1].dialog == "add_hotword"
    assert overlay.events[-1].payload["text"] == "Voicestick"


def test_框选添加_未选中报错发错误事件不弹窗(store):
    overlay = FakeOverlay()
    ctl = make_controller(
        store, StubSelection(error=ValueError("未选中文本（或选区为空白），无法添加热词")),
        overlay=overlay)
    ctl.on_add_selection()
    for _ in range(50):
        if overlay.events:
            break
        threading.Event().wait(0.02)
    assert overlay.events[-1].kind == "error"
    assert "未选中" in overlay.events[-1].text


def test_保存热词_manual来源_带读音变体(store):
    ctl = make_controller(store, StubSelection())
    ctl.save_hotword("Kubernetes", ["库伯内提斯", "kuernetes"])
    entry = store.get("Kubernetes")
    assert entry is not None
    assert entry.source == "manual"
    assert entry.pron == ["库伯内提斯", "kuernetes"]


# ---------- 改写确认入口 ----------

def test_确认最近口述_无缓存时发提示不弹窗(store):
    overlay = FakeOverlay()
    ctl = make_controller(store, StubSelection(), overlay=overlay)
    ctl.on_confirm_recent()
    assert overlay.events[-1].kind == "error"
    assert "最近" in overlay.events[-1].text


def test_确认最近口述_有缓存时发弹窗事件携带结果(store):
    overlay = FakeOverlay()
    ctl = make_controller(store, StubSelection(), overlay=overlay)
    ctl._process_worker_result(make_result())
    ctl.on_confirm_recent()
    assert overlay.events[-1].kind == "dialog"
    assert overlay.events[-1].dialog == "confirm_recent"
    assert overlay.events[-1].payload["result"].final_text == "我们打算迁移到Kubernetes。"


def test_保存纠正对_错读形式作为读音变体入库(store):
    ctl = make_controller(store, StubSelection())
    ctl._process_worker_result(make_result(corrections=[
        CorrectionEvent(wrong="库伯内提斯", right="Kubernetes", entry_id="x")]))
    ctl.save_corrections([("库伯内提斯", "Kubernetes")])
    entry = store.get("Kubernetes")
    assert entry is not None
    assert entry.source == "correction"
    assert "库伯内提斯" in entry.pron


def test_保存纠正对_空列表不动库(store):
    ctl = make_controller(store, StubSelection())
    ctl.save_corrections([])
    assert store.count() == 0


# ---------- 弹窗宿主（overlay dialog 事件的落地实现） ----------

def test_打开添加弹窗_绑定保存回调与词条文本(store, monkeypatch):
    created = {}

    class FakeDlg:
        def __init__(self, root, text, on_confirm):
            created["text"] = text
            created["on_confirm"] = on_confirm

    monkeypatch.setattr("p1.interaction.dialogs.HotwordDialog", FakeDlg)
    ctl = make_controller(store, StubSelection())
    ctl.open_add_hotword("Kubernetes")
    assert created["text"] == "Kubernetes"
    # 绑定的回调即 save_hotword：直接触发验证入库
    created["on_confirm"]("Kubernetes", ["kuernetes"])
    entry = store.get("Kubernetes")
    assert entry is not None and "kuernetes" in entry.pron


def test_打开确认弹窗_绑定保存回调与最近结果(store, monkeypatch):
    created = {}

    class FakeDlg:
        def __init__(self, root, result, on_save):
            created["result"] = result
            created["on_save"] = on_save

    monkeypatch.setattr("p1.interaction.dialogs.ConfirmRecentDialog", FakeDlg)
    ctl = make_controller(store, StubSelection())
    result = make_result()
    ctl.open_confirm_recent(result)
    assert created["result"] is result
    created["on_save"]([("库伯内提斯", "Kubernetes")])
    entry = store.get("Kubernetes")
    assert entry is not None and entry.source == "correction"


# ---------- 入口防抖（热键 down/up 双触发实测） ----------

def test_框选添加_防抖窗口内重复触发只处理一次(store):
    overlay = FakeOverlay()
    ctl = make_controller(store, StubSelection(text="Kubernetes"), overlay=overlay)
    ctl.on_add_selection()
    ctl.on_add_selection()  # 双触发（实测 keyboard 库 down/up 双匹配）
    for _ in range(50):
        if overlay.events:
            break
        threading.Event().wait(0.02)
    dialogs = [e for e in overlay.events if e.kind == "dialog"]
    assert len(dialogs) == 1


def test_改写确认_防抖窗口内重复触发只弹一窗(store):
    overlay = FakeOverlay()
    ctl = make_controller(store, StubSelection(), overlay=overlay)
    ctl._process_worker_result(make_result())
    ctl.on_confirm_recent()
    ctl.on_confirm_recent()
    dialogs = [e for e in overlay.events if e.kind == "dialog"]
    assert len(dialogs) == 1
