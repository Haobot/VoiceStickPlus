"""SenseVoice-Small int8 适配器（P1 默认本地引擎，纯 CPU 离线）。

sherpa-onnx 热词偏置仅支持 transducer，SenseVoice 无解码偏置——
热词纠正由编排层后处理负责（路线图 §6.2 SenseVoiceAdapter 设计）。
权重复用 m0/models/ 下载产物，不重复占用磁盘。
"""
from __future__ import annotations

import time
from pathlib import Path

import numpy as np

from p1.engines.base import EngineAdapter, EngineCapabilities, RecognizeResult, resolve_model_dir

_DIR_PATTERN = "sherpa-onnx-sense-voice-*"


class SenseVoiceAdapter(EngineAdapter):
    """SenseVoice-Small（zh/en/ja/ko/yue，int8 量化）。"""

    name = "sense_voice"

    def __init__(self, models_dir: str | Path, num_threads: int = 4):
        self._models_dir = Path(models_dir)
        self._num_threads = num_threads
        self._recognizer = None
        self.capabilities = EngineCapabilities(
            streaming=False, hotword_biasing="none",
            needs_gpu=False, license="Apache-2.0")

    def is_ready(self) -> bool:
        return any(self._models_dir.glob(_DIR_PATTERN))

    def load(self) -> None:
        if self._recognizer is not None:
            return
        if not self.is_ready():
            raise RuntimeError(
                f"sense_voice 模型未就位（{self._models_dir}）。"
                f"请先运行 m0/.venv/Scripts/python.exe "
                f"m0/scripts/download_models.py --only sense_voice")
        import sherpa_onnx

        model_dir = resolve_model_dir(self._models_dir, _DIR_PATTERN)
        self._recognizer = sherpa_onnx.OfflineRecognizer.from_sense_voice(
            model=str(model_dir / "model.int8.onnx"),
            tokens=str(model_dir / "tokens.txt"),
            num_threads=self._num_threads,
            use_itn=True,
        )

    def transcribe(self, samples: np.ndarray, sample_rate: int = 16000) -> RecognizeResult:
        if self._recognizer is None:
            self.load()
        # sherpa-onnx 统一吃 float32 归一化波形；int16/int32 按位深缩放
        if np.issubdtype(samples.dtype, np.integer):
            waveform = samples.astype(np.float32) / float(np.iinfo(samples.dtype).max)
        else:
            waveform = samples.astype(np.float32)
        stream = self._recognizer.create_stream()
        stream.accept_waveform(sample_rate, waveform)
        started = time.perf_counter()
        self._recognizer.decode_stream(stream)
        elapsed = time.perf_counter() - started
        return RecognizeResult(
            text=stream.result.text.strip(),
            elapsed_seconds=elapsed,
            audio_seconds=samples.size / sample_rate,
            confidence=None,
        )
