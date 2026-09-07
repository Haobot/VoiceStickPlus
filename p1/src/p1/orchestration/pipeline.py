"""识别编排管线：松手后 识别 → 热词纠正 → 飞轮回写 → AI 改写 → 注入。

每次 process 都从热词库实时构建纠正器——当句刚入库的热词下一句立即生效
（飞轮核心体验，见 test_动态热词_当句入库下一句生效）。
timing 记录各段耗时，total_seconds 即路线图 P1 的回填延迟验收口径。
"""
from __future__ import annotations

import time
from dataclasses import dataclass, field

import numpy as np

from p1.engines.base import EngineAdapter, RecognizeResult
from p1.flywheel.corrector import CorrectionEvent, HotwordCorrector, build_corrector_from_entries
from p1.flywheel.hotword_store import HotwordStore

# 短于该时长的采样视为误触，直接放弃
MIN_AUDIO_SECONDS = 0.2
# 每次识别注入纠正器的热词上限（候选过多拖慢后处理）
HOTWORD_TOP_N = 200


@dataclass
class PipelineResult:
    """一次口述的完整结果（悬浮条展示与延迟日志的数据源）。"""

    final_text: str
    raw_text: str
    injected: bool
    corrected_text: str = ""  # 热词纠正后、改写前的中间文本（三段对照用）
    corrections: list[CorrectionEvent] = field(default_factory=list)
    rewrite_corrections: list[str] = field(default_factory=list)
    rewrite_engine: str = ""
    timing: dict[str, float] = field(default_factory=dict)
    error: str = ""


class Pipeline:
    def __init__(self, engine: EngineAdapter, hotword_store: HotwordStore,
                 rewriter, injector, corrector_top_n: int = HOTWORD_TOP_N):
        self._engine = engine
        self._store = hotword_store
        self._rewriter = rewriter  # None = 关闭改写
        self._injector = injector
        self._top_n = corrector_top_n

    def process(self, samples: np.ndarray, sample_rate: int = 16000,
                inject_send_paste: bool = True) -> PipelineResult | None:
        timing: dict[str, float] = {}
        started = time.perf_counter()

        audio_seconds = samples.size / sample_rate
        if audio_seconds < MIN_AUDIO_SECONDS:
            return None

        # ① 识别
        t0 = time.perf_counter()
        result: RecognizeResult = self._engine.transcribe(samples, sample_rate)
        timing["recognize_seconds"] = time.perf_counter() - t0
        raw_text = result.text.strip()
        if not raw_text:
            return None

        # ② 热词纠正（纠正器实时构建，当句入库下一句生效）
        t0 = time.perf_counter()
        corrector = build_corrector_from_entries(self._store.top_n(self._top_n))
        text, corrections = corrector.correct(raw_text)
        corrected_text = text
        timing["correct_seconds"] = time.perf_counter() - t0
        for event in corrections:
            self._store.record_hit(event.right)  # 飞轮回写

        # ③ AI 改写（可选）
        rewrite_corrections: list[str] = []
        rewrite_engine = ""
        t0 = time.perf_counter()
        if self._rewriter is not None:
            rewritten = self._rewriter.rewrite(text)
            text = rewritten.text
            rewrite_corrections = rewritten.corrections
            rewrite_engine = rewritten.engine
        timing["rewrite_seconds"] = time.perf_counter() - t0

        # ④ 注入（失败不抛异常，结果里带错误供悬浮条提示）
        t0 = time.perf_counter()
        injected, error = True, ""
        try:
            self._injector.inject(text, send_paste=inject_send_paste,
                                  restore_clipboard=inject_send_paste)
        except Exception as exc:
            injected, error = False, f"{type(exc).__name__}: {exc}"
        timing["inject_seconds"] = time.perf_counter() - t0
        timing["total_seconds"] = time.perf_counter() - started

        return PipelineResult(
            final_text=text, raw_text=raw_text, injected=injected,
            corrected_text=corrected_text,
            corrections=corrections, rewrite_corrections=rewrite_corrections,
            rewrite_engine=rewrite_engine, timing=timing, error=error)
