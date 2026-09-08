"""本地引擎：FunASR SeACo-Paraformer（双层热词管线，可选评测项）。"""
from __future__ import annotations

import time
from pathlib import Path

from m0_asr import model_registry
from m0_asr.providers.base import AsrProvider, HealthStatus, TranscribeOutcome

# 与 hotword_seaco.py 相同的后处理阈值（见 docs/pitfalls.md 坑 11）
POSTPROCESS_THRESHOLD = 0.85


class LocalSeacoProvider(AsrProvider):
    """FunASR SeACo-Paraformer + 文本级热词后处理（pytorch 栈，较重）。"""

    name = "local_seaco"
    kind = "local"

    def __init__(self, hotwords_file: Path | None = None):
        self.hotwords_file = hotwords_file
        self._model = None

    def health_check(self) -> HealthStatus:
        spec = model_registry.FUNASR_SEACO
        if not spec.is_ready():
            return HealthStatus(ok=False,
                                detail=f"模型未就位: {spec.dir}（scripts/download_models.py --only funasr_seaco）")
        return HealthStatus(ok=True, detail="模型就位（pytorch + 热词双层）")

    def _ensure_loaded(self):
        if self._model is None:
            from funasr import AutoModel
            self._model = AutoModel(model=str(model_registry.FUNASR_SEACO.dir),
                                    disable_update=True)

    def warmup(self) -> None:
        self._ensure_loaded()

    def transcribe_file(self, wav_path: str | Path) -> TranscribeOutcome:
        try:
            self._ensure_loaded()
            kwargs = {"input": str(wav_path)}
            if self.hotwords_file and Path(self.hotwords_file).exists():
                kwargs["hotword"] = str(self.hotwords_file)
                kwargs["postprocess_hotword_file"] = str(self.hotwords_file)
                kwargs["postprocess_hotword_threshold"] = POSTPROCESS_THRESHOLD
            started = time.perf_counter()
            res = self._model.generate(**kwargs)
            elapsed = time.perf_counter() - started
            text = res[0]["text"]
            # funasr 返回无稳定时长字段，本地读取计算
            from m0_asr.audio_utils import read_wave
            samples, sr = read_wave(wav_path)
            return TranscribeOutcome(
                text=text,
                elapsed_seconds=elapsed,
                audio_seconds=samples.size / sr,
            )
        except Exception as exc:
            return TranscribeOutcome(text="", elapsed_seconds=0.0, audio_seconds=0.0,
                                     error=f"{type(exc).__name__}: {exc}")
