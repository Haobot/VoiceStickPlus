"""ASR Provider 抽象基类（路线图 §6.2 EngineAdapter 的 M0 具象）。

约定：
- health_check 只验证可用性（本地=权重就位；云端=凭据完整），不发起识别请求；
- transcribe_file 端到端计时（调用开始→拿到文本），云端含网络往返——
  与用户体感一致，跨引擎对比用同一口径；
- 失败不抛异常，返回 Outcome(error=...)，由评测管线统一记失败句。
"""
from __future__ import annotations

from abc import ABC, abstractmethod
from dataclasses import dataclass
from pathlib import Path

from m0_asr.metrics import rtf


@dataclass(frozen=True)
class HealthStatus:
    """引擎可用性检查结果。"""

    ok: bool
    detail: str


@dataclass(frozen=True)
class TranscribeOutcome:
    """单句识别结果（端到端口径）。"""

    text: str
    elapsed_seconds: float
    audio_seconds: float
    error: str | None = None

    @property
    def rtf(self) -> float:
        return rtf(self.elapsed_seconds, self.audio_seconds)


class AsrProvider(ABC):
    """统一 ASR 引擎接口：编排层只面向本接口，不直接调用引擎。"""

    name: str = ""
    kind: str = ""  # "local" | "cloud"

    @abstractmethod
    def health_check(self) -> HealthStatus:
        """可用性检查（不识别）。"""

    @abstractmethod
    def transcribe_file(self, wav_path: str | Path) -> TranscribeOutcome:
        """识别单个 16k 单声道 wav（其他采样率由实现自行重采样）。"""

    # 实现可在首次 transcribe 时惰性加载（重量级资源不进构造器）
    def warmup(self) -> None:  # pragma: no cover - 可选钩子
        """可选预热（加载模型/建立连接）。"""
