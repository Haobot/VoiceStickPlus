"""engine 模块单元测试：识别流程编排 / 热词格式 / 模型就绪校验。

真实模型加载与识别精度由集成脚本（transcribe/benchmark/hotword_*）验证，
本文件只测可离线复现的纯逻辑。
"""
import numpy as np
import pytest
import soundfile as sf

from m0_asr import engine
from m0_asr.engine import TranscribeResult, format_hotwords, transcribe


# ---------- Fake 组件：记录调用、返回固定结果 ----------

class FakeStream:
    def __init__(self, hotwords):
        self.hotwords = hotwords
        self.sample_rate = None
        self.samples = None
        self.result = type("R", (), {"text": "你好世界"})()

    def accept_waveform(self, sample_rate, samples):
        self.sample_rate = sample_rate
        self.samples = samples


class FakeRecognizer:
    def __init__(self):
        self.streams = []

    def create_stream(self, hotwords=None):
        stream = FakeStream(hotwords)
        self.streams.append(stream)
        return stream

    def decode_stream(self, stream):
        pass


def _write_wav(path, sample_rate: int, seconds: float = 0.5) -> None:
    t = np.arange(int(seconds * sample_rate)) / sample_rate
    sf.write(path, (0.5 * np.sin(2 * np.pi * 440 * t)).astype(np.float32), sample_rate)


# ---------- format_hotwords ----------

def test_format_hotwords_斜杠连接():
    assert format_hotwords(["Kubernetes", "梓骞"]) == "Kubernetes/梓骞"


def test_format_hotwords_空列表返回None():
    assert format_hotwords([]) is None
    assert format_hotwords(None) is None


def test_format_hotwords_单个词():
    assert format_hotwords(["WebSocket"]) == "WebSocket"


# ---------- transcribe ----------

def test_transcribe_返回文本与指标(tmp_path):
    wav = tmp_path / "a.wav"
    _write_wav(wav, 16000, seconds=0.5)
    result = transcribe(FakeRecognizer(), wav)
    assert isinstance(result, TranscribeResult)
    assert result.text == "你好世界"
    assert abs(result.audio_seconds - 0.5) < 0.01
    assert result.elapsed_seconds >= 0.0
    assert result.rtf >= 0.0


def test_transcribe_热词传递到stream(tmp_path):
    wav = tmp_path / "a.wav"
    _write_wav(wav, 16000)
    fake = FakeRecognizer()
    transcribe(fake, wav, hotwords=["Kubernetes", "WebSocket"])
    assert fake.streams[0].hotwords == "Kubernetes/WebSocket"


def test_transcribe_无热词时hotwords为None(tmp_path):
    wav = tmp_path / "a.wav"
    _write_wav(wav, 16000)
    fake = FakeRecognizer()
    transcribe(fake, wav)
    assert fake.streams[0].hotwords is None


def test_transcribe_非16k自动重采样(tmp_path):
    wav = tmp_path / "a48k.wav"
    _write_wav(wav, 48000, seconds=1.0)
    fake = FakeRecognizer()
    transcribe(fake, wav)
    stream = fake.streams[0]
    assert stream.sample_rate == 16000
    assert abs(len(stream.samples) / 16000 - 1.0) < 0.01


# ---------- 模型就绪校验 ----------

def test_create_sense_voice_模型缺失时抛错(monkeypatch):
    from m0_asr import model_registry
    fake_spec = model_registry.ModelSpec(
        key="sense_voice",
        package="不存在的包",
        required_files=("model.int8.onnx",),
        license_name="Apache-2.0",
        license_note="仅测试用",
    )
    monkeypatch.setattr(engine, "SENSE_VOICE", fake_spec)
    with pytest.raises(RuntimeError, match="sense_voice"):
        engine.create_sense_voice_recognizer()
