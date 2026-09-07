"""引擎适配器测试：能力声明单测 + SenseVoice 真模型集成（未就位 SKIP 不伪造）。"""
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf

from p1.engines.base import EngineCapabilities, EngineAdapter
from p1.engines.sense_voice import SenseVoiceAdapter

M0_ROOT = Path(__file__).resolve().parent.parent.parent / "m0"
MODELS_DIR = M0_ROOT / "models"
WAV = M0_ROOT / "data" / "wavs" / "daily_01.wav"

MODEL_READY = any(MODELS_DIR.glob("sherpa-onnx-sense-voice-*"))


# ---------- 能力声明（纯单测） ----------

def test_SenseVoice能力声明_无热词偏置走后处理():
    caps = SenseVoiceAdapter(models_dir=MODELS_DIR).capabilities
    assert isinstance(caps, EngineCapabilities)
    assert caps.hotword_biasing == "none"  # 路线图 §6.2：纠错放后处理管线
    assert caps.streaming is False
    assert caps.needs_gpu is False
    assert caps.license == "Apache-2.0"


def test_适配器是EngineAdapter子类():
    assert issubclass(SenseVoiceAdapter, EngineAdapter)
    assert SenseVoiceAdapter(models_dir=MODELS_DIR).name == "sense_voice"


def test_模型目录不存在_is_ready为False且load报清晰错误(tmp_path):
    adapter = SenseVoiceAdapter(models_dir=tmp_path)
    assert adapter.is_ready() is False
    with pytest.raises(RuntimeError, match="download_models"):
        adapter.load()


# ---------- 真模型集成（模型未就位整体 SKIP，不 mock） ----------

@pytest.mark.skipif(not MODEL_READY, reason="SenseVoice 模型未下载")
class TestSenseVoiceRealModel:
    def test_真识别_中文日常句(self):
        adapter = SenseVoiceAdapter(models_dir=MODELS_DIR)
        adapter.load()
        samples, sr = sf.read(WAV, dtype="int16")
        result = adapter.transcribe(samples, sr)
        assert result.audio_seconds > 3
        assert result.elapsed_seconds > 0
        # daily_01 GT: 今天中午吃什么……牛肉面
        assert "面条" in result.text or "牛肉面" in result.text or "吃什么" in result.text

    def test_识别结果字段完整(self):
        adapter = SenseVoiceAdapter(models_dir=MODELS_DIR)
        adapter.load()
        samples, sr = sf.read(WAV, dtype="int16")
        result = adapter.transcribe(samples, sr)
        assert isinstance(result.text, str) and result.text
        assert result.confidence is None  # SenseVoice 不输出置信度（M0 结论）
        assert result.rtf >= 0

    def test_重复调用无需重新加载(self):
        adapter = SenseVoiceAdapter(models_dir=MODELS_DIR)
        adapter.load()
        first = id(adapter._recognizer)
        adapter.load()
        assert id(adapter._recognizer) == first

    def test_float采样自动转int16口径(self):
        adapter = SenseVoiceAdapter(models_dir=MODELS_DIR)
        adapter.load()
        samples, sr = sf.read(WAV, dtype="float32")
        int16_samples, _ = sf.read(WAV, dtype="int16")
        # 两种 dtype 输入识别结果一致（适配器内部归一化）
        assert adapter.transcribe(samples, sr).text == \
               adapter.transcribe(int16_samples, sr).text
