"""compare_report 单测：延迟分布统计 / 指标聚合 / 报告渲染（喂构造数据）。"""
from m0_asr.compare_report import (
    EngineMetrics,
    aggregate,
    latency_stats,
    render_report,
)


# ---------- latency_stats ----------

def test_延迟统计_基本量():
    stats = latency_stats([0.1, 0.2, 0.3, 0.4, 5.0])
    assert stats["median"] == 0.3
    assert stats["mean"] > 0
    # P95 应显著高于中位（有一个 5s 离群）
    assert stats["p95"] > stats["median"]


def test_延迟统计_空列表():
    stats = latency_stats([])
    assert stats["median"] == 0.0
    assert stats["p95"] == 0.0


# ---------- aggregate ----------

def _outcome_rows(engine, latencies, cers, hotword_miss=None):
    rows = []
    for i, (lat, cer) in enumerate(zip(latencies, cers)):
        rows.append({
            "engine": engine,
            "id": f"t{i}",
            "category": "daily_zh",
            "hotword": None if hotword_miss is None else ("Kubernetes" if i % 2 else None),
            "text_gt": "甲" * 10,
            "text_asr": "乙" * 10 if cer > 0 else "甲" * 10,
            "elapsed_seconds": lat,
            "audio_seconds": 5.0,
            "hit": None if hotword_miss is None else (i >= hotword_miss),
            "error": None,
        })
    return rows


def test_aggregate_完整指标():
    rows = _outcome_rows("e1", [0.1, 0.2, 0.3], [0.0, 0.1, 0.2], hotword_miss=1)
    metrics = aggregate("e1", rows)
    assert isinstance(metrics, EngineMetrics)
    assert metrics.total == 3
    assert metrics.failures == 0
    assert metrics.cer_overall > 0
    assert metrics.hotword_total == 2
    assert metrics.hotword_misses == 1
    assert metrics.error_rate == 0.5


def test_aggregate_含失败句():
    rows = _outcome_rows("e1", [0.1, 0.2], [0.0, 0.0])
    rows[1]["error"] = "boom"
    metrics = aggregate("e1", rows)
    assert metrics.failures == 1
    # 失败句不计入 CER
    assert metrics.total == 1


# ---------- render_report ----------

def test_渲染报告_含关键段落():
    rows = _outcome_rows("local", [0.1] * 3, [0.0] * 3)
    metrics = {"local": aggregate("local", rows)}
    report = render_report(metrics, dataset_note="测试")
    assert "local" in report
    assert "CER" in report
    assert "结论" in report or "建议" in report


def test_渲染报告_无结果有说明():
    report = render_report({}, dataset_note="空")
    assert "无" in report or "未" in report
