"""热词后处理纠正器单测：SenseVoice 无解码偏置，纠正全靠本模块（路线图 §6.2）。

测试数据取自 M0 四引擎评测的真实错例（kuernetes / 大小写 / 中文读音变体）。
"""
from p1.flywheel.corrector import HotwordCorrector, build_corrector_from_entries
from p1.flywheel.hotword_store import HotwordEntry


def _entry(surface, pron=None):
    return HotwordEntry(id=f"id-{surface}", surface=surface,
                        pron=list(pron or []), source="manual", freq=5,
                        weight=1.2, last_used="2026-09-07")


# ---------- 基础行为 ----------

def test_无热词_原样返回():
    corrected, events = HotwordCorrector([]).correct("今天天气不错")
    assert corrected == "今天天气不错"
    assert events == []


def test_精确匹配_仅大小写纠正():
    c = HotwordCorrector([_entry("Kubernetes")])
    corrected, events = c.correct("我们迁移到kubernetes集群")
    assert "Kubernetes" in corrected
    assert len(events) == 1
    assert events[0].wrong == "kubernetes" and events[0].right == "Kubernetes"


# ---------- M0 真实错例 ----------

def test_编辑距离纠正_M0实测错例_kuernetes():
    c = HotwordCorrector([_entry("Kubernetes")])
    corrected, events = c.correct("我们打算把整个服务迁移到kuernetes集群上")
    assert corrected == "我们打算把整个服务迁移到Kubernetes集群上"
    assert len(events) == 1


def test_中文读音变体纠正_库伯内提斯到Kubernetes():
    c = HotwordCorrector([_entry("Kubernetes", pron=["库伯内提斯"])])
    corrected, events = c.correct("服务都跑在库伯内提斯上面")
    assert corrected == "服务都跑在Kubernetes上面"
    assert events[0].right == "Kubernetes"


def test_多热词同句纠正():
    c = HotwordCorrector([_entry("Kubernetes"), _entry("WebSocket")])
    corrected, events = c.correct("kuernetes和websocket都要升级")
    assert corrected == "Kubernetes和WebSocket都要升级"
    assert len(events) == 2


# ---------- 误伤控制 ----------

def test_低相似片段不纠正_避免误伤():
    c = HotwordCorrector([_entry("Kubernetes")])
    corrected, events = c.correct("今天天气很好我们去公园散步吧")
    assert corrected == "今天天气很好我们去公园散步吧"
    assert events == []


def test_长度差异过大跳过fuzzy():
    c = HotwordCorrector([_entry("SOTA")])
    corrected, events = c.correct("这是一个超长的英文单词concatenation不该被替换")
    assert corrected == "这是一个超长的英文单词concatenation不该被替换"
    assert events == []


def test_阈值之下不纠正():
    c = HotwordCorrector([_entry("Redis")])
    # red1s 与 redis 相似度 ~0.89 会替换；rxxxxs 相似度过低不替换
    corrected, events = c.correct("连接rxxxxs失败")
    assert corrected == "连接rxxxxs失败"
    assert events == []


# ---------- 工厂与事件结构 ----------

def test_工厂_从热词库条目构建并展开读音变体():
    entries = [_entry("Kubernetes", pron=["库伯内提斯", "k8s"])]
    c = build_corrector_from_entries(entries)
    # pron 变体与 surface 都进入候选读音
    corrected, _ = c.correct("kuernetes与库伯内提斯")
    assert corrected.count("Kubernetes") == 2
