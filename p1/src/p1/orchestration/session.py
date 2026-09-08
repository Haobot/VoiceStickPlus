"""录音会话：AudioBuffer（纯逻辑，可测）+ RecordingSession（sounddevice 薄包装）。

按住说话模式：start() 开流，音频回调只做 push 与电平计算（快，不掉块）；
stop() 关流并交出完整采样。设备异常时向调用方抛错，不静默吞。
"""
from __future__ import annotations

from collections import deque

import numpy as np

SAMPLE_RATE = 16000
BLOCK_MS = 40
DEFAULT_MAX_SECONDS = 120


class AudioBuffer:
    """线程安全的采样收集器（sounddevice 回调线程写，stop 后主线程读）。"""

    def __init__(self, sample_rate: int = SAMPLE_RATE, max_seconds: int = DEFAULT_MAX_SECONDS):
        self.sample_rate = sample_rate
        self._max_samples = max_seconds * sample_rate
        self._chunks: deque[np.ndarray] = deque()

    def push(self, chunk: np.ndarray) -> None:
        self._chunks.append(chunk)
        # 挂键防呆：只保留尾部窗口，超长自动丢弃早期数据
        total = sum(c.size for c in self._chunks)
        while total > self._max_samples and len(self._chunks) > 1:
            total -= self._chunks.popleft().size
        if total > self._max_samples:
            # 单块自身超限（如异常大块）：截取尾部
            keep = self._max_samples
            self._chunks.clear()
            self._chunks.append(chunk[-keep:])

    def level(self) -> float:
        """最近一块的 RMS 电平（悬浮条动态反馈用）。"""
        if not self._chunks:
            return 0.0
        last = self._chunks[-1].astype(np.float64)
        return float(np.sqrt(np.mean(np.square(last))))

    def duration_seconds(self) -> float:
        return sum(c.size for c in self._chunks) / self.sample_rate

    def flush(self) -> np.ndarray:
        samples = (np.concatenate(self._chunks) if self._chunks
                   else np.zeros(0, dtype=np.int16))
        self._chunks.clear()
        return samples


class RecordingSession:
    """按住说话会话（对 sounddevice InputStream 的薄包装，外部边界）。"""

    def __init__(self, sample_rate: int = SAMPLE_RATE, device: int | None = None):
        self.sample_rate = sample_rate
        self.device = device
        self.buffer = AudioBuffer(sample_rate)
        self._stream = None

    def start(self) -> None:
        import sounddevice

        if self._stream is not None:
            return
        self.buffer = AudioBuffer(self.sample_rate)
        self._stream = sounddevice.InputStream(
            samplerate=self.sample_rate,
            channels=1,
            dtype="int16",
            blocksize=self.sample_rate * BLOCK_MS // 1000,
            device=self.device,
            callback=lambda data, frames, time_info, status: self.buffer.push(data[:, 0].copy()),
        )
        self._stream.start()

    def stop(self) -> np.ndarray:
        """关流并返回完整采样（int16 单声道）。"""
        if self._stream is None:
            return np.zeros(0, dtype=np.int16)
        self._stream.stop()
        self._stream.close()
        self._stream = None
        return self.buffer.flush()

    @property
    def is_recording(self) -> bool:
        return self._stream is not None
