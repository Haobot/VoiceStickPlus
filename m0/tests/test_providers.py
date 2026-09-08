"""providers 单测：注册表 / 健康检查逻辑 / Outcome 数据结构。

不依赖真实模型权重与真实云端调用（那属于 sim_compare 集成验证），
这里只测注册与状态判定逻辑。
"""
import pytest

from m0_asr.providers import get_provider, registered_provider_names
from m0_asr.providers.base import AsrProvider, HealthStatus, TranscribeOutcome


# ---------- 注册表 ----------

def test_注册表包含四个引擎():
    names = registered_provider_names()
    assert "local_sense_voice" in names
    assert "local_seaco" in names
    assert "cloud_tencent" in names
    assert "cloud_tencent_hotword" in names  # 云端临时热词表变体


def test_工厂_未知引擎抛错():
    with pytest.raises(KeyError):
        get_provider("不存在的引擎", credentials=None)


def test_工厂_云端热词变体_读热词文件():
    from m0_asr.providers.cloud_tencent import CloudTencentProvider
    provider = get_provider("cloud_tencent_hotword", credentials=None)
    assert isinstance(provider, CloudTencentProvider)
    assert provider.name == "cloud_tencent_hotword"
    assert provider.hotword_list  # 从 data/texts/hotwords.txt 读到了热词


def test_工厂_本地引擎可实例化():
    provider = get_provider("local_sense_voice", credentials=None)
    assert isinstance(provider, AsrProvider)
    assert provider.kind == "local"


def test_工厂_热词引擎注入同一份热词源():
    # local_seaco 与 cloud_tencent_hotword 必须同源（data/texts/hotwords.txt），保证公平对比
    seaco = get_provider("local_seaco", credentials=None)
    assert seaco.hotwords_file is not None, "工厂未给 local_seaco 注入热词文件"
    cloud_hw = get_provider("cloud_tencent_hotword", credentials=None)
    assert cloud_hw.hotword_list


# ---------- HealthStatus / Outcome ----------

def test_health_status_字段():
    status = HealthStatus(ok=True, detail="模型就位")
    assert status.ok is True
    status2 = HealthStatus(ok=False, detail="凭据缺失")
    assert status2.ok is False


def test_outcome_成功与失败形态():
    ok = TranscribeOutcome(text="你好", elapsed_seconds=0.1, audio_seconds=1.0)
    assert ok.error is None
    assert ok.rtf == pytest.approx(0.1)
    failed = TranscribeOutcome(text="", elapsed_seconds=0.2, audio_seconds=1.0,
                               error="timeout")
    assert failed.error == "timeout"


# ---------- cloud_tencent 健康检查（不发起真实调用） ----------

def test_cloud_tencent_无凭据_健康检查失败():
    from m0_asr.providers.cloud_tencent import CloudTencentProvider
    provider = CloudTencentProvider(credentials=None)
    status = provider.health_check()
    assert status.ok is False
    assert "凭据" in status.detail


def test_cloud_tencent_有凭据_健康检查通过():
    from m0_asr.providers.cloud_tencent import CloudTencentProvider
    from m0_asr.platform_config import CloudTencentCredentials
    cred = CloudTencentCredentials(secret_id="id", secret_key="key", appid="1",
                                   engine_model_type="16k_zh")
    provider = CloudTencentProvider(credentials=cred)
    status = provider.health_check()
    assert status.ok is True


# ---------- local 健康检查（模型目录不存在时） ----------

def test_local_sense_voice_模型缺失_健康检查失败(monkeypatch, tmp_path):
    from m0_asr import model_registry
    fake = model_registry.ModelSpec(
        key="sense_voice", package="不存在", required_files=("model.int8.onnx",),
        license_name="Apache-2.0", license_note="t")
    monkeypatch.setattr(model_registry, "SENSE_VOICE", fake)
    provider = get_provider("local_sense_voice", credentials=None)
    status = provider.health_check()
    assert status.ok is False
