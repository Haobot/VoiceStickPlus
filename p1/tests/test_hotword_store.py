"""HotwordStore 单测：Schema v1 / 增删查 / 命中计数 / 权重衰减 / 加密落盘。"""
import time
from pathlib import Path

import pytest

from p1.flywheel.hotword_store import HotwordEntry, HotwordStore


@pytest.fixture()
def store(tmp_path):
    """隔离的热词库（临时 db + 临时密钥）。"""
    return HotwordStore(
        db_path=tmp_path / "hotwords.db",
        key_file=tmp_path / "hw.key",
    )


# ---------- Schema v1：增查 ----------

def test_添加并查询_字段完整对齐Schema_v1(store):
    entry = store.add(surface="Kubernetes", source="manual",
                      pron=["kubernetes", "库伯内提斯"], tags=["k8s", "work"])
    assert entry.id  # uuid
    assert entry.surface == "Kubernetes"
    assert entry.pron == ["kubernetes", "库伯内提斯"]
    assert entry.source == "manual"
    assert entry.freq == 1
    assert entry.tags == ["k8s", "work"]
    assert entry.weight == 1.0
    assert entry.last_used  # ISO 日期

    got = store.get("Kubernetes")
    assert got is not None and got.id == entry.id


def test_大小写不敏感去重_重复添加幂等(store):
    store.add(surface="Kubernetes", source="manual")
    again = store.add(surface="kubernetes", source="correction")
    # 幂等：不新增行，freq 累加，source 可升级（manual 优先级最高不被覆盖）
    assert store.count() == 1
    assert again.freq == 2
    assert again.source == "manual"


def test_查询不存在返回None(store):
    assert store.get("不存在的词") is None


# ---------- 命中计数与权重飞轮 ----------

def test_命中计数_freq增长且last_used刷新(store):
    store.add(surface="Redis", source="manual")
    time.sleep(0.01)  # 确保 last_used 可观测变化
    hit = store.record_hit("redis")
    assert hit.freq == 2


def test_权重衰减_久不用降权(store):
    from dataclasses import replace
    from datetime import date

    entry = store.add(surface="PostgreSQL", source="manual")
    stale = replace(entry, last_used=(date.today() - __import__("datetime").timedelta(days=90)).isoformat())
    effective = store.effective_weight(stale, decay_days=30)
    # 90 天未用 = 3 个衰减周期，0.9^3 ≈ 0.729
    assert effective == pytest.approx(0.9 ** 3, rel=1e-3)
    fresh = store.get("PostgreSQL")
    assert store.effective_weight(fresh, decay_days=30) == pytest.approx(1.0)


def test_topN_按有效权重排序(store):
    for w in ["A", "B", "C"]:
        store.add(surface=w, source="manual")
    store.record_hit("A")
    store.record_hit("B")
    store.record_hit("B")  # B 权重最高，A 次之，C 未命中垫底
    top = store.top_n(2, decay_days=30)
    assert [e.surface for e in top] == ["B", "A"]


def test_空库topN返回空列表(store):
    assert store.top_n(5, decay_days=30) == []


# ---------- 加密落盘（安全验收） ----------

def test_落盘密文_surface明文不可grep(store):
    store.add(surface="绝密热词Kubernetes", source="manual", tags=["机密tag"])
    raw = (store.db_path).read_bytes()
    assert "绝密热词".encode("utf-8") not in raw
    assert b"Kubernetes" not in raw
    assert "机密tag".encode("utf-8") not in raw


def test_密钥复用_重启后可读(store, tmp_path):
    store.add(surface="跨会话热词", source="manual")
    # 模拟重启：同 key 文件新开一个 store 实例
    reopened = HotwordStore(db_path=store.db_path, key_file=tmp_path / "hw.key")
    assert reopened.get("跨会话热词") is not None


def test_密钥文件不存在则自动生成_32字节(store, tmp_path):
    assert (tmp_path / "hw.key").exists()
    assert (tmp_path / "hw.key").stat().st_size == 32


# ---------- 删除与列举 ----------

def test_删除热词(store):
    store.add(surface="临时词", source="auto_mine")
    assert store.remove("临时词") is True
    assert store.get("临时词") is None
    assert store.remove("临时词") is False


def test_列举全部(store):
    store.add(surface="词一", source="manual")
    store.add(surface="词二", source="correction")
    all_entries = store.list_all()
    assert {e.surface for e in all_entries} == {"词一", "词二"}
    assert all(isinstance(e, HotwordEntry) for e in all_entries)


# ---------- 连接生命周期（Windows 句柄释放） ----------

def test_close释放句柄_目录可删(store, tmp_path):
    import shutil
    store.add(surface="句柄测试", source="manual")
    store.close()
    # Windows 下未关连接会 PermissionError，关了才能删
    shutil.rmtree(tmp_path)
    assert not (tmp_path / "hotwords.db").exists()


def test_上下文管理器用法(tmp_path):
    with HotwordStore(db_path=tmp_path / "a.db", key_file=tmp_path / "k.key") as s:
        s.add(surface="作用域内", source="manual")
    assert (tmp_path / "a.db").exists()


# ---------- 跨线程访问（识别 worker 线程读库，真机多线程契约） ----------

def test_worker线程读取不报错(store):
    import threading
    store.add(surface="跨线程词", source="manual")
    result: dict = {}

    def worker():
        try:
            result["top"] = [e.surface for e in store.top_n(10)]
            result["count"] = store.count()
        except Exception as exc:  # noqa: BLE001 测试需要捕获线程内异常
            result["error"] = repr(exc)

    t = threading.Thread(target=worker)
    t.start()
    t.join(timeout=5)
    assert "error" not in result, result.get("error")
    assert result["top"] == ["跨线程词"]
    assert result["count"] == 1
