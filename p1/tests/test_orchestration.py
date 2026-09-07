"""录音缓冲与编排管线单测：AudioBuffer 纯逻辑 + Pipeline 编排（协作者替身）。"""
import numpy as np
import pytest

from p1.orchestration.session import AudioBuffer
from p1.orchestration.pipeline import Pipeline, PipelineResult
from p1.flywheel.hotword_store import HotwordStore


# ---------- AudioBuffer ----------

def test_缓冲区_推块时长与采样数():
    buf = AudioBuffer(sample_rate=16000)
    buf.push(np.ones(1600, dtype=np.int16) * 3000)  # 0.1s
    buf.push(np.ones(8000, dtype=np.int16) * 3000)  # 0.5s
    assert buf.duration_seconds() == pytest.approx(0.6)  # flush 前测时长
    samples = buf.flush()
    assert samples.size == 9600


def test_缓冲区_电平计算():
    buf = AudioBuffer(sample_rate=16000)
    assert buf.level() == 0.0  # 空缓冲电平 0
    buf.push(np.ones(1600, dtype=np.int16) * 3000)
    assert buf.level() > 0.0
    buf.flush()
    assert buf.level() == 0.0  # flush 后归零


def test_缓冲区_静音块电平为0():
    buf = AudioBuffer(sample_rate=16000)
    buf.push(np.zeros(1600, dtype=np.int16))
    assert buf.level() == 0.0


def test_缓冲区_时长过长自动截断():
    # 防呆：录音超过 120s 只保留尾部（异常挂住热键的兜底）
    buf = AudioBuffer(sample_rate=16000, max_seconds=120)
    buf.push(np.zeros(16000 * 130, dtype=np.int16))
    assert buf.duration_seconds() <= 120.0


# ---------- Pipeline 编排（协作者替身） ----------

class FakeEngine:
    def __init__(self, text):
        self._text = text
        self.calls = 0

    def transcribe(self, samples, sample_rate=16000):
        self.calls += 1
        from p1.engines.base import RecognizeResult
        return RecognizeResult(text=self._text, elapsed_seconds=0.01,
                               audio_seconds=1.0, confidence=None)


class FakeInjector:
    def __init__(self, fail=False):
        self.injected_texts = []
        self.fail = fail

    def inject(self, text):
        if self.fail:
            raise RuntimeError("目标窗口不可用")
        self.injected_texts.append(text)


class FakeRewriter:
    def __init__(self):
        self.calls = []

    def rewrite(self, text):
        self.calls.append(text)
        from p1.rewrite.rewriter import RewriteResult
        return RewriteResult(text=text + "。", changed=True, engine="rules")


@pytest.fixture()
def store(tmp_path):
    return HotwordStore(db_path=tmp_path / "hw.db", key_file=tmp_path / "hw.key")


def _samples(seconds=1.0):
    return (np.random.default_rng(42).standard_normal(int(16000 * seconds)) * 3000
            ).astype(np.int16)


def test_正常流_识别纠正改写注入全链路(store):
    store.add(surface="Kubernetes", source="manual")
    pipeline = Pipeline(
        engine=FakeEngine("我们迁移到kuernetes集群"),
        hotword_store=store,
        rewriter=FakeRewriter(),
        injector=FakeInjector(),
    )
    result = pipeline.process(_samples())
    assert isinstance(result, PipelineResult)
    assert result.injected is True
    assert result.final_text == "我们迁移到Kubernetes集群。"
    assert len(result.corrections) == 1
    assert result.timing["total_seconds"] >= 0


def test_纠正命中回写热词库_飞轮转动(store):
    entry = store.add(surface="Kubernetes", source="manual")
    assert entry.freq == 1
    pipeline = Pipeline(
        engine=FakeEngine("kuernetes上线"),
        hotword_store=store,
        rewriter=None,  # 关闭改写
        injector=FakeInjector(),
    )
    pipeline.process(_samples())
    after = store.get("Kubernetes")
    assert after.freq == 2  # 命中一次


def test_空音频放弃_不调引擎():
    engine = FakeEngine("不该被调用")
    pipeline = Pipeline(engine=engine, hotword_store=store, rewriter=None,
                        injector=FakeInjector())
    result = pipeline.process(np.zeros(1600, dtype=np.int16))  # 0.1s
    assert result is None
    assert engine.calls == 0


def test_空文本不注入():
    pipeline = Pipeline(engine=FakeEngine(""), hotword_store=store,
                        rewriter=None, injector=FakeInjector())
    result = pipeline.process(_samples())
    assert result is None


def test_注入失败_结果标记错误不抛异常(store):
    pipeline = Pipeline(engine=FakeEngine("你好"), hotword_store=store,
                        rewriter=None, injector=FakeInjector(fail=True))
    result = pipeline.process(_samples())
    assert result.injected is False
    assert "目标窗口不可用" in result.error


def test_关闭改写_不调rewriter(store):
    rewriter = FakeRewriter()
    pipeline = Pipeline(engine=FakeEngine("你好"), hotword_store=store,
                        rewriter=None, injector=FakeInjector())
    pipeline.process(_samples())
    assert rewriter.calls == []


def test_计时明细齐全(store):
    pipeline = Pipeline(engine=FakeEngine("你好"), hotword_store=store,
                        rewriter=None, injector=FakeInjector())
    result = pipeline.process(_samples())
    for key in ("recognize_seconds", "correct_seconds", "rewrite_seconds",
                "inject_seconds", "total_seconds"):
        assert key in result.timing
        assert result.timing[key] >= 0


def test_动态热词_当句入库下一句生效(store):
    # 飞轮核心体验：识别后添加的热词，无需重启下一句就参与纠正
    pipeline = Pipeline(engine=FakeEngine("kuernetes上线"), hotword_store=store,
                        rewriter=None, injector=FakeInjector())
    result1 = pipeline.process(_samples())
    assert "Kubernetes" not in result1.final_text  # 尚无热词，未纠正
    store.add(surface="Kubernetes", source="manual")
    result2 = pipeline.process(_samples())
    assert "Kubernetes" in result2.final_text  # 立即生效


# ---------- PipelineResult 中间文本（改写确认三段对照） ----------

def test_pipeline结果_带纠正后中间文本():
    from p1.orchestration.pipeline import PipelineResult
    r = PipelineResult(final_text="最终", raw_text="原文",
                       corrected_text="纠正后", injected=True)
    assert r.corrected_text == "纠正后"


def test_pipeline结果_中间文本默认空_兼容旧构造():
    from p1.orchestration.pipeline import PipelineResult
    r = PipelineResult(final_text="f", raw_text="r", injected=True)
    assert r.corrected_text == ""
