"""控制器状态机：热键事件 → 录音会话 → 识别管线 → 悬浮条反馈 的粘合层。

线程契约（谁在哪个线程）：
- 热键回调（keyboard 钩子线程）：start_session / finish_session / cancel_session
- 识别 worker（每次口述一个短命线程）：_process_worker
- tkinter 主线程：只收 OverlayEvent 渲染

识别进行中（busy）忽略新会话，避免并发识别把 UI 状态打乱。
"""
from __future__ import annotations

import logging
import threading
import time

from p1.interaction.overlay import OverlayApp, OverlayEvent
from p1.orchestration.pipeline import Pipeline
from p1.orchestration.session import RecordingSession

log = logging.getLogger("p1.controller")


class VoiceController:
    def __init__(self, session: RecordingSession, pipeline: Pipeline, overlay: OverlayApp):
        self._session = session
        self._pipeline = pipeline
        self._overlay = overlay
        self._busy = threading.Lock()  # 识别 worker 与新会话互斥
        self._recording = False
        self._record_started = 0.0

    # ---- 热键回调（钩子线程，快速返回） ----

    def start_session(self) -> None:
        if self._recording or not self._busy.acquire(blocking=False):
            return
        try:
            self._session.start()
            self._recording = True
            self._record_started = time.perf_counter()
            self._overlay.post(OverlayEvent(kind="recording", detail="松开出字"))
        except Exception as exc:  # 设备故障不静默：报错并释放
            self._busy.release()
            log.exception("录音启动失败")
            self._overlay.post(OverlayEvent(kind="error", text=f"录音启动失败: {exc}"))

    def finish_session(self) -> None:
        if not self._recording:
            return
        self._recording = False
        samples = self._session.stop()
        self._overlay.post(OverlayEvent(
            kind="recognizing",
            detail=f"录音 {time.perf_counter() - self._record_started:.1f}s"))
        threading.Thread(target=self._process_worker, args=(samples,), daemon=True).start()

    def cancel_session(self) -> None:
        if self._recording:
            self._session.stop()
            self._recording = False
            self._busy.release()
        self._overlay.post(OverlayEvent(kind="hide"))

    # ---- 识别 worker（短命线程） ----

    def _process_worker(self, samples) -> None:
        try:
            result = self._pipeline.process(samples)
            if result is None:
                log.info("本句无有效内容（时长 %.2fs），跳过",
                         samples.size / 16000)
                self._overlay.post(OverlayEvent(kind="hide"))
                return
            t = result.timing
            log.info(
                "口述完成 total=%.3fs recognize=%.3fs correct=%.3fs rewrite=%.3fs "
                "inject=%.3fs rtf_overall=%.2f corrections=%d rewrite=[%s] text=%r",
                t.get("total_seconds", 0), t.get("recognize_seconds", 0),
                t.get("correct_seconds", 0), t.get("rewrite_seconds", 0),
                t.get("inject_seconds", 0),
                t.get("total_seconds", 0) / max(0.001, samples.size / 16000),
                len(result.corrections), ",".join(result.rewrite_corrections),
                result.final_text)
            if result.injected:
                self._overlay.post(OverlayEvent(
                    kind="result", text=result.final_text,
                    corrections=tuple((c.wrong, c.right) for c in result.corrections),
                    detail=f"{t.get('total_seconds', 0):.2f}s"))
            else:
                self._overlay.post(OverlayEvent(
                    kind="error", text=f"注入失败：{result.error}（文本已生成）",
                    detail=f"{t.get('total_seconds', 0):.2f}s"))
        except Exception:
            log.exception("识别管线异常")
            self._overlay.post(OverlayEvent(kind="error", text="识别异常，已放弃本句"))
        finally:
            self._busy.release()
