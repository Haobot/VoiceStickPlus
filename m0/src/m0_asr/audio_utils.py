"""音频工具：wav 读取（折叠单声道）、线性插值重采样。"""
from __future__ import annotations

from pathlib import Path

import numpy as np
import soundfile as sf


def read_wave(path: str | Path) -> tuple[np.ndarray, int]:
    """读 wav，多声道折叠为单声道均值，返回 (float32 单声道, 采样率)。"""
    if not Path(path).exists():
        raise FileNotFoundError(f"音频文件不存在: {path}")
    data, sample_rate = sf.read(str(path), dtype="float32", always_2d=True)
    mono = data.mean(axis=1).astype(np.float32)
    return mono, sample_rate


def resample(samples: np.ndarray, src_rate: int, dst_rate: int) -> np.ndarray:
    """线性插值重采样。同采样率直接返回，非法采样率抛 ValueError。"""
    if src_rate <= 0 or dst_rate <= 0:
        raise ValueError(f"采样率必须为正: src={src_rate}, dst={dst_rate}")
    if src_rate == dst_rate or samples.size == 0:
        return samples.astype(np.float32, copy=False)
    duration = samples.size / src_rate
    dst_len = max(1, int(round(duration * dst_rate)))
    src_pos = np.arange(dst_len) * (src_rate / dst_rate)
    return np.interp(src_pos, np.arange(samples.size), samples).astype(np.float32)
