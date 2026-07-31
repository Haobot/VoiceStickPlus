"""评测指标：CER（字准）、延迟分位数、跨轮稳定性统计。"""
from __future__ import annotations

import unicodedata
from statistics import median

from .result import ClipResult

# 中文数字 -> 阿拉伯数字（ASR 常把「一二三」识别为「123」，归一化消除该系统性差异）
_ZH_DIGITS = {
    "零": "0", "一": "1", "二": "2", "两": "2", "三": "3", "四": "4",
    "五": "5", "六": "6", "七": "7", "八": "8", "九": "9",
}


def normalize_text(text: str, zh_digits: bool = True) -> str:
    """归一化用于 CER 比较：去标点/空白/大小写/全半角，可选中文数字转阿拉伯。"""
    out = []
    for ch in text:
        ch = unicodedata.normalize("NFKC", ch)
        if not ch:
            continue
        c = ch[0]
        cat = unicodedata.category(c)
        if cat.startswith("P") or cat.startswith("Z") or cat.startswith("S"):
            continue  # 标点 / 空白 / 符号（含 % $ 等）不参与字准
        if zh_digits and c in _ZH_DIGITS:
            c = _ZH_DIGITS[c]
        out.append(c.lower())
    return "".join(out)


def levenshtein(a: str, b: str) -> int:
    """编辑距离（单条语料长度有限，O(n*m) 足够）。"""
    if len(a) < len(b):
        a, b = b, a
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[-1] + 1, prev[j - 1] + (ca != cb)))
        prev = cur
    return prev[-1]


def cer(reference: str, hypothesis: str) -> float:
    """字符错误率；参考归一化后为空时返回 0。"""
    ref = normalize_text(reference)
    hyp = normalize_text(hypothesis)
    if not ref:
        return 0.0
    return levenshtein(ref, hyp) / len(ref)


def percentile(values: list[float], p: float) -> float | None:
    """最近秩分位数；空列表返回 None。"""
    if not values:
        return None
    ordered = sorted(values)
    rank = max(0, min(len(ordered) - 1, round(p / 100.0 * (len(ordered) - 1))))
    return ordered[rank]


def summarize(results: list[ClipResult]) -> dict:
    """单 provider 全部轮次汇总指标。"""
    ok = [r for r in results if r.success]
    failed = [r for r in results if not r.success]
    cers = [cer(r.reference, r.final_text) for r in ok]
    first_lat = [r.first_partial_latency_ms for r in ok
                 if r.first_partial_latency_ms is not None]
    tail_lat = [r.tail_latency_ms for r in ok if r.tail_latency_ms is not None]
    total_lat = [r.total_latency_ms for r in ok if r.total_latency_ms is not None]

    # 按类别拆 CER
    by_cat: dict[str, list[float]] = {}
    for r, c in zip(ok, cers):
        by_cat.setdefault(r.category or "uncategorized", []).append(c)

    # 跨轮结果抖动：同一条语料不同轮次识别文本两两 CER 的最大值
    by_clip: dict[str, list[ClipResult]] = {}
    for r in ok:
        by_clip.setdefault(r.clip_id, []).append(r)
    jitters = []
    for clip_results in by_clip.values():
        texts = [normalize_text(r.final_text) for r in clip_results]
        worst = 0.0
        for i in range(len(texts)):
            for j in range(i + 1, len(texts)):
                base = max(len(texts[i]), len(texts[j]), 1)
                worst = max(worst, levenshtein(texts[i], texts[j]) / base)
        if len(texts) > 1:
            jitters.append(worst)

    error_kinds: dict[str, int] = {}
    for r in failed:
        kind = r.error.split(":", 1)[0] if r.error else "unknown"
        error_kinds[kind] = error_kinds.get(kind, 0) + 1
    for r in results:
        for _ in r.server_errors:
            error_kinds["server_error"] = error_kinds.get("server_error", 0) + 1

    return {
        "runs": len(results),
        "success": len(ok),
        "failed": len(failed),
        "failure_rate": round(len(failed) / len(results), 4) if results else None,
        "cer_mean": round(sum(cers) / len(cers), 4) if cers else None,
        "cer_median": round(median(cers), 4) if cers else None,
        "cer_max": round(max(cers), 4) if cers else None,
        "cer_by_category": {k: round(sum(v) / len(v), 4) for k, v in sorted(by_cat.items())},
        "first_partial_latency_ms": {
            "p50": percentile(first_lat, 50), "p95": percentile(first_lat, 95),
        },
        "tail_latency_ms": {
            "p50": percentile(tail_lat, 50), "p95": percentile(tail_lat, 95),
        },
        "total_latency_ms": {
            "p50": percentile(total_lat, 50), "p95": percentile(total_lat, 95),
        },
        "jitter_mean": round(sum(jitters) / len(jitters), 4) if jitters else None,
        "jitter_max": round(max(jitters), 4) if jitters else None,
        "error_kinds": error_kinds,
    }


def per_clip_detail(results: list[ClipResult]) -> list[dict]:
    """逐条语料明细：各轮 CER、最终文本、失败原因，供报告下钻。"""
    detail = []
    for r in results:
        detail.append({
            "clip_id": r.clip_id,
            "provider": r.provider,
            "round": r.round,
            "category": r.category,
            "success": r.success,
            "error": r.error,
            "server_errors": r.server_errors,
            "cer": round(cer(r.reference, r.final_text), 4) if r.success else None,
            "reference": r.reference,
            "final_text": r.final_text,
            "first_partial_latency_ms": r.first_partial_latency_ms,
            "tail_latency_ms": r.tail_latency_ms,
        })
    return detail
