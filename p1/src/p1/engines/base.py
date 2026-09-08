"""引擎适配器抽象（路线图 §6.2 EngineAdapter 的 P1 落地）。

编排层（Pipeline）只面向本接口编程，永远不直接调用具体引擎。
"""
from __future__ import annotations

from abc import ABC, abstractmethod
from dataclasses import dataclass
from pathlib import Path

import numpy as np


@dataclass(frozen=True)
class EngineCapabilities:
    """引擎能力自描述（编排层据此决定热词注入策略与引擎分级）。"""

    streaming: bool
    hotword_biasing: str  # "none" | "static_list" | "context_prompt"
    needs_gpu: bool
    license: str


@dataclass(frozen=True)
class RecognizeResult:
    """单次识别结果；confidence 为 None 表示引擎不输出置信度。"""

    text: str
    elapsed_seconds: float
    audio_seconds: float
    confidence: float | None = None

    @property
    def rtf(self) -> float:
        if self.audio_seconds <= 0:
            return 0.0
        return self.elapsed_seconds / self.audio_seconds


class EngineAdapter(ABC):
    """统一引擎接口：is_ready 检查 / load 显式加载 / transcribe 识别。"""

    name: str = ""
    capabilities: EngineCapabilities

    @abstractmethod
    def is_ready(self) -> bool:
        """依赖（模型权重等）是否就位，不触发加载。"""

    @abstractmethod
    def load(self) -> None:
        """加载引擎（重复调用幂等）。"""

    @abstractmethod
    def transcribe(self, samples: np.ndarray, sample_rate: int = 16000) -> RecognizeResult:
        """识别 PCM 采样（int16 或 float32，单声道），端到端计时。"""

    def warmup(self) -> None:  # pragma: no cover - 可选钩子
        """可选预热（首句识别前的模型热身）。"""


def resolve_model_dir(models_dir: str | Path, pattern: str) -> Path:
    """在模型根目录下按 glob 模式定位具体引擎目录（容忍版本号后缀变化）。"""
    root = Path(models_dir)
    matches = sorted(root.glob(pattern))
    if not matches:
        raise RuntimeError(
            f"模型未就位：{root} 下找不到 {pattern}。"
            f"请先运行 m0/scripts/download_models.py 下载权重。")
    return matches[0]
