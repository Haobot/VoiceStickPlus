"""评测结果数据结构。"""
from __future__ import annotations

from dataclasses import dataclass, field


@dataclass
class ClipResult:
    """单条语料单轮回放结果。延迟单位 ms；失败时 success=False 且 error 非空。"""
    clip_id: str
    provider: str
    round: int
    category: str = ""
    reference: str = ""
    duration_s: float = 0.0
    success: bool = False
    error: str = ""
    final_text: str = ""
    # 从首发音频帧到首个非空 partial 的延迟
    first_partial_latency_ms: float | None = None
    # 从末帧音频发完到最终结果（final/session_finished）的尾延迟
    tail_latency_ms: float | None = None
    # 从首发音频帧到最终结果的总耗时
    total_latency_ms: float | None = None
    partial_count: int = 0
    server_errors: list[str] = field(default_factory=list)

    def to_dict(self) -> dict:
        return {
            "clip_id": self.clip_id,
            "provider": self.provider,
            "round": self.round,
            "category": self.category,
            "reference": self.reference,
            "duration_s": round(self.duration_s, 3),
            "success": self.success,
            "error": self.error,
            "final_text": self.final_text,
            "first_partial_latency_ms": self.first_partial_latency_ms,
            "tail_latency_ms": self.tail_latency_ms,
            "total_latency_ms": self.total_latency_ms,
            "partial_count": self.partial_count,
            "server_errors": self.server_errors,
        }
