"""本地引擎封装：三个引擎的加载工厂 + 统一识别流程。

引擎与用途（M0）：
- sense_voice     提示词 A 主引擎（基准测试 / CER 基线）
- seaco_paraformer 提示词 B 路线一（SeACo 热词偏置）
- qwen3_asr        提示词 B 路线二（context prompt 热词）

识别流程约定：
- 统一重采样到 16kHz 单声道后送入引擎；
- 计时口径 = decode_stream 耗时（不含模型加载与音频 IO）；
- 热词以 per-stream 方式注入（create_stream(hotwords="词1/词2")，
  由 sherpa-onnx 分发给支持偏置的模型）。
"""
from __future__ import annotations

import time
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Optional

import sherpa_onnx

from m0_asr.audio_utils import read_wave, resample
from m0_asr.metrics import rtf
from m0_asr.model_registry import QWEN3_ASR, SEACO_PARAFORMER, SENSE_VOICE

TARGET_SAMPLE_RATE = 16000

if TYPE_CHECKING:  # 仅类型标注用，避免运行时依赖
    from numpy import ndarray


@dataclass(frozen=True)
class TranscribeResult:
    """单次识别结果与计时指标。"""

    text: str
    audio_seconds: float
    elapsed_seconds: float

    @property
    def rtf(self) -> float:
        return rtf(self.elapsed_seconds, self.audio_seconds)


def format_hotwords(words: Optional[list[str]]) -> Optional[str]:
    """热词列表 → sherpa-onnx per-stream 格式（`/` 分隔）；空列表返回 None。"""
    if not words:
        return None
    return "/".join(words)


def _require_model(spec, what: str) -> None:
    """模型未就位时给出带下载指引的错误。"""
    if not spec.is_ready():
        raise RuntimeError(
            f"{what} 模型未就位（{spec.dir}）。"
            f"请先运行: .venv/Scripts/python.exe scripts/download_models.py --only {spec.key}"
        )


def create_sense_voice_recognizer(num_threads: int = 4) -> sherpa_onnx.OfflineRecognizer:
    """SenseVoice-Small int8（提示词 A 主引擎）。"""
    _require_model(SENSE_VOICE, "sense_voice")
    return sherpa_onnx.OfflineRecognizer.from_sense_voice(
        model=str(SENSE_VOICE.dir / "model.int8.onnx"),
        tokens=str(SENSE_VOICE.dir / "tokens.txt"),
        num_threads=num_threads,
        use_itn=True,
    )


def create_seaco_recognizer(num_threads: int = 4) -> sherpa_onnx.OfflineRecognizer:
    """SeACo-Paraformer 中英粤三语（提示词 B 路线一，原生热词偏置）。"""
    _require_model(SEACO_PARAFORMER, "seaco_paraformer")
    return sherpa_onnx.OfflineRecognizer.from_paraformer(
        paraformer=str(SEACO_PARAFORMER.dir / "model.int8.onnx"),
        tokens=str(SEACO_PARAFORMER.dir / "tokens.txt"),
        num_threads=num_threads,
    )


def create_qwen3_recognizer(num_threads: int = 4, hotwords_csv: str = "") -> sherpa_onnx.OfflineRecognizer:
    """Qwen3-ASR 0.6B int8（提示词 B 路线二，context 偏置）。

    hotwords_csv: 逗号分隔热词（官方构造级参数，写入 context prompt），
    如 "Kubernetes,WebSocket,SOTA"。
    """
    _require_model(QWEN3_ASR, "qwen3_asr")
    return sherpa_onnx.OfflineRecognizer.from_qwen3_asr(
        conv_frontend=str(QWEN3_ASR.dir / "conv_frontend.onnx"),
        encoder=str(QWEN3_ASR.dir / "encoder.int8.onnx"),
        decoder=str(QWEN3_ASR.dir / "decoder.int8.onnx"),
        tokenizer=str(QWEN3_ASR.dir / "tokenizer"),
        num_threads=num_threads,
        hotwords=hotwords_csv,
    )


def transcribe(
    recognizer,
    wav_path: str | Path,
    hotwords: Optional[list[str]] = None,
) -> TranscribeResult:
    """识别单个 wav：读取 → 16k 重采样 → 注入热词 → 计时解码。"""
    samples, sample_rate = read_wave(wav_path)
    if sample_rate != TARGET_SAMPLE_RATE:
        samples = resample(samples, sample_rate, TARGET_SAMPLE_RATE)

    stream = recognizer.create_stream(hotwords=format_hotwords(hotwords))
    stream.accept_waveform(TARGET_SAMPLE_RATE, samples)

    started = time.perf_counter()
    recognizer.decode_stream(stream)
    elapsed = time.perf_counter() - started

    audio_seconds = samples.size / TARGET_SAMPLE_RATE
    return TranscribeResult(
        text=stream.result.text,
        audio_seconds=audio_seconds,
        elapsed_seconds=elapsed,
    )
