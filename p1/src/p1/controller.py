"""控制器状态机：热键事件 → 录音会话 → 识别管线 → 悬浮条反馈 的粘合层。

线程契约（谁在哪个线程）：
- 热键回调（keyboard 钩子线程）：start_session / finish_session / cancel_session
  / on_add_selection / on_confirm_recent（后两者立即转工作线程，不在钩子线程睡等）
- 识别 worker（每次口述一个短命线程）：_process_worker
- tkinter 主线程：只收 OverlayEvent 渲染；dialog 弹窗在主线程创建，
  用户确认后经 controller 的 save_* 方法回写热词库（主线程直调，SQLite 已开跨线程）。

识别进行中（busy）忽略新会话，避免并发识别把 UI 状态打乱。
"""
from __future__ import annotations

import logging
import threading
import time

from p1.interaction.overlay import OverlayApp, OverlayEvent
from p1.orchestration.pipeline import Pipeline, PipelineResult
from p1.orchestration.session import RecordingSession

log = logging.getLogger("p1.controller")

# 入口防抖窗口：keyboard 库组合热键存在 down/up 双匹配（实测同一组合连触发两次）
ENTRY_DEBOUNCE_SECONDS = 1.0


class VoiceController:
    def __init__(self, session: RecordingSession, pipeline: Pipeline, overlay: OverlayApp,
                 hotword_store=None, selection_reader=None):
        self._session = session
        self._pipeline = pipeline
        self._overlay = overlay
        self._store = hotword_store
        self._selection = selection_reader
        self._busy = threading.Lock()  # 识别 worker 与新会话互斥
        self._recording = False
        self._record_started = 0.0
        self._last_result: PipelineResult | None = None
        self._last_add_at = 0.0   # 入口防抖时间戳
        self._last_confirm_at = 0.0

    @property
    def last_result(self) -> PipelineResult | None:
        """最近一次口述结果（改写确认入口的数据源，新口述覆盖）。"""
        return self._last_result

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

    # ---- 热词两个入口（钩子线程入口，转工作线程） ----

    def on_add_selection(self) -> None:
        now = time.perf_counter()
        if now - self._last_add_at < ENTRY_DEBOUNCE_SECONDS:
            return
        self._last_add_at = now
        log.info("框选添加入口触发")
        threading.Thread(target=self._add_selection_worker, daemon=True).start()

    def _add_selection_worker(self) -> None:
        try:
            text = self._selection.read_selection()
        except ValueError as exc:
            log.warning("框选添加放弃: %s", exc)
            self._overlay.post(OverlayEvent(kind="error", text=str(exc)))
            return
        except Exception:
            log.exception("读取选中文本失败")
            self._overlay.post(OverlayEvent(kind="error", text="读取选中文本失败"))
            return
        log.info("框选读取到 %d 字符，弹窗确认", len(text))
        self._overlay.post(OverlayEvent(
            kind="dialog", dialog="add_hotword", payload={"text": text}))

    def on_confirm_recent(self) -> None:
        now = time.perf_counter()
        if now - self._last_confirm_at < ENTRY_DEBOUNCE_SECONDS:
            return
        self._last_confirm_at = now
        log.info("改写确认入口触发（%s）",
                 "有最近口述" if self._last_result is not None else "无最近口述")
        if self._last_result is None:
            self._overlay.post(OverlayEvent(
                kind="error", text="没有最近的口述可确认"))
            return
        self._overlay.post(OverlayEvent(
            kind="dialog", dialog="confirm_recent",
            payload={"result": self._last_result}))

    # ---- 弹窗宿主（overlay dialog 事件转发，tk 主线程调用） ----

    def open_add_hotword(self, text: str) -> None:
        from p1.interaction.dialogs import HotwordDialog
        HotwordDialog(self._overlay.root(), text, on_confirm=self.save_hotword)

    def open_confirm_recent(self, result: PipelineResult) -> None:
        from p1.interaction.dialogs import ConfirmRecentDialog
        ConfirmRecentDialog(self._overlay.root(), result, on_save=self.save_corrections)

    # ---- 弹窗确认回调（tk 主线程） ----

    def save_hotword(self, surface: str, prons: list[str]) -> None:
        entry = self._store.add(surface.strip(), source="manual",
                                pron=[p.strip() for p in prons if p.strip()])
        log.info("热词入库(manual): %s（读音变体 %d 个）", entry.surface, len(entry.pron))
        self._overlay.post(OverlayEvent(kind="hide"))

    def save_corrections(self, pairs: list[tuple[str, str]]) -> None:
        for wrong, right in pairs:
            entry = self._store.add(right, source="correction", pron=[wrong])
            log.info("纠正确认入库: %s ← 错读[%s]", entry.surface, wrong)
        if pairs:
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
            self._process_worker_result(result, samples)
        except Exception:
            log.exception("识别管线异常")
            self._overlay.post(OverlayEvent(kind="error", text="识别异常，已放弃本句"))
        finally:
            self._busy.release()

    def _process_worker_result(self, result: PipelineResult, samples=None) -> None:
        self._last_result = result
        t = result.timing
        audio_seconds = samples.size / 16000 if samples is not None else 1.0
        log.info(
            "口述完成 total=%.3fs recognize=%.3fs correct=%.3fs rewrite=%.3fs "
            "inject=%.3fs rtf_overall=%.2f corrections=%d rewrite=[%s] text=%r",
            t.get("total_seconds", 0), t.get("recognize_seconds", 0),
            t.get("correct_seconds", 0), t.get("rewrite_seconds", 0),
            t.get("inject_seconds", 0),
            t.get("total_seconds", 0) / max(0.001, audio_seconds),
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
