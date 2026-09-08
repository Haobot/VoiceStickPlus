"""audio_utils 模块单元测试：wav 读取 / 单声道折叠 / 重采样。"""
import numpy as np
import pytest
import soundfile as sf

from m0_asr.audio_utils import read_wave, resample


def _sine(freq: float, seconds: float, sample_rate: int) -> np.ndarray:
    t = np.arange(int(seconds * sample_rate)) / sample_rate
    return (0.5 * np.sin(2 * np.pi * freq * t)).astype(np.float32)


# ---------- read_wave ----------

def test_read_wave_单声道16k(tmp_path):
    wav = tmp_path / "mono.wav"
    samples = _sine(440, 0.5, 16000)
    sf.write(wav, samples, 16000)
    got, sr = read_wave(wav)
    assert sr == 16000
    assert got.ndim == 1
    assert len(got) == 8000
    assert got.dtype == np.float32


def test_read_wave_立体声折叠为单声道(tmp_path):
    wav = tmp_path / "stereo.wav"
    left = _sine(440, 0.5, 16000)
    stereo = np.stack([left, np.zeros_like(left)], axis=1)
    sf.write(wav, stereo, 16000)
    got, sr = read_wave(wav)
    assert sr == 16000
    assert got.ndim == 1
    # 右声道全零 → 折叠后幅度减半
    assert np.allclose(got, left / 2, atol=1e-4)


def test_read_wave_文件不存在(tmp_path):
    with pytest.raises(FileNotFoundError):
        read_wave(tmp_path / "nope.wav")


# ---------- resample ----------

def test_resample_同采样率原样返回():
    samples = _sine(440, 1.0, 16000)
    out = resample(samples, 16000, 16000)
    assert len(out) == len(samples)


def test_resample_48k降到16k():
    samples = _sine(440, 1.0, 48000)
    out = resample(samples, 48000, 16000)
    assert len(out) == 16000


def test_resample_频率保持():
    # 重采样后 440Hz 正弦的过零点数量应大致不变（频率保持）
    samples = _sine(440, 1.0, 48000)
    out = resample(samples, 48000, 16000)
    crossings_before = np.sum(np.diff(np.signbit(samples)) != 0)
    crossings_after = np.sum(np.diff(np.signbit(out)) != 0)
    assert abs(crossings_before - crossings_after) <= 4


def test_resample_空输入():
    out = resample(np.array([], dtype=np.float32), 16000, 48000)
    assert len(out) == 0


def test_resample_非法采样率():
    samples = _sine(440, 0.1, 16000)
    with pytest.raises(ValueError):
        resample(samples, 0, 16000)
    with pytest.raises(ValueError):
        resample(samples, 16000, -1)
