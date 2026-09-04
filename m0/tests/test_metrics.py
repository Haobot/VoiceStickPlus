"""metrics 模块单元测试：CER / 文本归一化 / 热词命中错误率 / 降幅。

覆盖：正常流、边界条件（空串、大小写、标点、全半角）、错误路径。
"""
from m0_asr.metrics import (
    cer,
    hotword_error_stats,
    hotword_hit,
    normalize_for_cer,
    normalize_for_match,
    reduction_rate,
    rtf,
)


# ---------- normalize_for_cer ----------

def test_normalize_for_cer_去空格与标点():
    assert normalize_for_cer("你好，世界！") == "你好世界"
    assert normalize_for_cer("Hello,  World.") == "helloworld"


def test_normalize_for_cer_全角转半角():
    # 全角字母数字应折叠为半角，保证中英混读口径一致
    assert normalize_for_cer("Ｋ8ｓ") == "k8s"


def test_normalize_for_cer_大小写折叠():
    assert normalize_for_cer("Kubernetes") == "kubernetes"


def test_normalize_for_cer_空串():
    assert normalize_for_cer("") == ""
    assert normalize_for_cer("。，！") == ""


# ---------- normalize_for_match ----------

def test_normalize_for_match_与_cer_口径一致():
    # 热词命中判定用同一归一化，避免两套口径
    assert normalize_for_match("Kubernetes，") == normalize_for_cer("kubernetes")


# ---------- cer ----------

def test_cer_完全一致为0():
    assert cer("今天天气不错", "今天天气不错") == 0.0


def test_cer_单字替换():
    # 6 字参考，1 字替换 → 1/6
    assert abs(cer("今天天气不错", "今天天氧不错") - 1 / 6) < 1e-9


def test_cer_插入与删除():
    # 参考 4 字，假设 5 字（多 1 插入）→ 0.25
    assert abs(cer("语音识别", "语音识辨别") - 0.25) < 1e-9
    # 参考 4 字，假设 3 字（少 1 删除）→ 0.25
    assert abs(cer("语音识别", "语音别") - 0.25) < 1e-9


def test_cer_标点大小写不参与计错():
    assert cer("你好，World！", "你好 world") == 0.0


def test_cer_参考为空():
    # 边界：参考为空、假设非空 → 满错（1.0）；两者皆空 → 0
    assert cer("", "") == 0.0
    assert cer("", "abc") == 1.0


def test_cer_中英混读():
    # 混读场景：错一个英文词
    er = cer("用 Kubernetes 部署服务", "用 kubernates 部署服务")
    assert 0.0 < er < 1.0


# ---------- rtf ----------

def test_rtf_基本计算():
    assert abs(rtf(elapsed_seconds=0.5, audio_seconds=10.0) - 0.05) < 1e-12


def test_rtf_音频时长为零():
    # 防除零：静音/空音频返回 0.0
    assert rtf(elapsed_seconds=0.5, audio_seconds=0.0) == 0.0


def test_rtf_负数时长():
    assert rtf(elapsed_seconds=-1.0, audio_seconds=10.0) == 0.0
    assert rtf(elapsed_seconds=0.5, audio_seconds=-3.0) == 0.0


# ---------- hotword_hit ----------

def test_hotword_hit_精确命中():
    assert hotword_hit("我们用 Kubernetes 部署", "Kubernetes") is True


def test_hotword_hit_大小写不敏感():
    assert hotword_hit("我们用 kubernetes 部署", "Kubernetes") is True


def test_hotword_hit_标点粘连不影响():
    assert hotword_hit("使用Kubernetes，很方便", "Kubernetes") is True


def test_hotword_hit_未命中():
    assert hotword_hit("我们用库伯内提斯部署", "Kubernetes") is False


def test_hotword_hit_子串误配不算命中():
    # "Kube" 不是 "Kubernetes" 的命中（防误报：目标词必须完整出现）
    assert hotword_hit("我们用 Kube 部署", "Kubernetes") is False


def test_hotword_hit_中文热词():
    assert hotword_hit("同事梓骞负责这一块", "梓骞") is True
    assert hotword_hit("同事子谦负责这一块", "梓骞") is False


# ---------- hotword_error_stats ----------

def test_hotword_error_stats_统计与错误率():
    stats = hotword_error_stats(
        samples=[
            ("我们用 Kubernetes 部署", "Kubernetes"),   # 命中
            ("我们用库伯内提斯部署", "Kubernetes"),      # 未命中
            ("同事梓骞负责", "梓骞"),                     # 命中
        ]
    )
    assert stats.total == 3
    assert stats.hits == 2
    assert abs(stats.error_rate - 1 / 3) < 1e-9


def test_hotword_error_stats_空样本():
    stats = hotword_error_stats(samples=[])
    assert stats.total == 0
    assert stats.error_rate == 0.0


def test_hotword_error_stats_全错与全对():
    all_miss = hotword_error_stats([("甲", "Kubernetes"), ("乙", "Kubernetes")])
    assert all_miss.error_rate == 1.0
    all_hit = hotword_error_stats([("Kubernetes", "Kubernetes")] * 4)
    assert all_hit.error_rate == 0.0


# ---------- reduction_rate ----------

def test_reduction_rate_基本降幅():
    assert abs(reduction_rate(baseline=0.8, biased=0.4) - 0.5) < 1e-12


def test_reduction_rate_基线为零():
    # 基线本来就没错 → 降幅无意义，返回 0.0（不抛异常）
    assert reduction_rate(baseline=0.0, biased=0.0) == 0.0
    assert reduction_rate(baseline=0.0, biased=0.5) == 0.0


def test_reduction_rate_偏置后反而变差():
    # 负降幅如实返回
    assert reduction_rate(baseline=0.4, biased=0.8) == -1.0
