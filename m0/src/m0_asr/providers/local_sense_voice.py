"""本地引擎：SenseVoice int8（sherpa-onnx）。"""
from __future__ import annotations

import time
from pathlib import Path

from m0_asr import model_registry
from m0_asr.audio_utils import read_wave, resample
from m0_asr.providers.base import AsrProvider, HealthStatus, TranscribeOutcome


class LocalSenseVoiceProvider(AsrProvider):
    """SenseVoice-Small int8，纯 CPU，M0 主引擎。"""

    name = "local_sense_voice"
    kind = "local"

    def __init__(self, num_threads: int = 4):
        self.num_threads = num_threads
        self._recognizer = None  # 惰性加载

    def health_check(self) -> HealthStatus:
        spec = model_registry.SENSE_VOICE
        if not spec.is_ready():
            return HealthStatus(ok=False,
                                detail=f"模型未就位: {spec.dir}（scripts/download_models.py --only sense_voice）")
        return HealthStatus(ok=True, detail="模型就位（int8）")

    def _ensure_loaded(self):
        if self._recognizer is None:
            from m0_asr.engine import create_sense_voice_recognizer
            self._recognizer = create_sense_voice_recognizer(num_threads=self.num_threads)

    def warmup(self) -> None:
        self._ensure_loaded()

    def transcribe_file(self, wav_path: str | Path) -> TranscribeOutcome:
        try:
            self._ensure_loaded()
            samples, sample_rate = read_wave(wav_path)
            if sample_rate != 16000:
                samples = resample(samples, sample_rate, 16000)
            started = time.perf_counter()
            stream = self._recognizer.create_stream()
            stream.accept_waveform(16000, samples)
            self._recognizer.decode_stream(stream)
            elapsed = time.perf_counter() - started
            return TranscribeOutcome(
                text=stream.result.text,
                elapsed_seconds=elapsed,
                audio_seconds=samples.size / 16000,
            )
        except Exception as exc:  # 失败句由管线统一记账
            return TranscribeOutcome(text="", elapsed_seconds=0.0, audio_seconds=0.0,
                                     error=f"{type(exc).__name__}: {exc}")
