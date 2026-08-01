"""高频热词优先排序与平台预算裁剪（纯 stdlib，无第三方依赖）。

背景：用户热词库持续增长，两平台会话级直传容量有限（火山双向流式
corpus.context 约 100 tokens，腾讯临时热词表 128 词）。词库超限时按
「频率 × 新近度 × 手动加权」评分裁剪，尽量保住高频热词的识别精度。

评分模型与桌面端约定（Doc/Plan/hotword-eval-and-prioritization.md §3）：
    score = 1.0 * log(1 + count) + 0.5 * exp(-(now - last_used) / 30d)
            + 2.0 * (source == "manual")
token 估算规则与 desktop/windows/src/asr_protocol.cc 的
EstimateHotwordTokens 保持一致：CJK 每字 1 token，ASCII 每 3 字符 1 token。

自测：python hotword_select.py
"""
from __future__ import annotations

import json
import math
import time
from dataclasses import dataclass, field

# 评分权重（默认）：手动加词基本必胜，高频常用词次之，长尾旧词淘汰。
W_COUNT = 1.0
W_RECENCY = 0.5
W_MANUAL = 2.0
RECENCY_TAU_S = 30.0 * 86400.0

# 平台限额（调研结论见 Doc/Plan/hotword-eval-and-prioritization.md §1）。
VOLC_STREAM_TOKEN_BUDGET = 80   # 官方 100 tokens，留 20 余量（同桌面端 kHotwordCorpusTokenBudget）
VOLC_STREAM_MAX_WORD_TOKENS = 10
TENCENT_TEMP_LIMIT = 128        # 临时热词表词数上限
TENCENT_TABLE_LIMIT = 1000      # 热词表每表词数上限
VOLC_TABLE_LIMIT = 5000         # 火山热词表每表词数上限
MAX_CJK_CHARS = 10              # 两平台共同的单词长度上限：≤10 汉字
MAX_ASCII_CHARS = 30            # ≤30 英文字符


@dataclass
class HotwordStat:
    """单个热词的使用统计。count=历史出现/使用次数；last_used_ts=最近使用
    时间戳（0=未知，按最旧处理）；source="manual"（用户手动加词）或 "mined"
    （候选挖掘/精修 diff）。"""
    word: str
    count: int = 0
    last_used_ts: float = 0.0
    source: str = "mined"


def estimate_tokens(word: str) -> int:
    """与 asr_protocol.cc::EstimateHotwordTokens 一致的粗略估算。"""
    cjk = sum(1 for ch in word if ord(ch) >= 0x80)
    ascii_chars = len(word) - cjk
    tokens = cjk + (ascii_chars + 2) // 3
    return max(tokens, 1 if word else 0)


def is_valid_word(word: str) -> bool:
    """两平台共同的硬约束：不含空格，≤10 汉字 / ≤30 英文字符。"""
    if not word or any(ch.isspace() for ch in word):
        return False
    cjk = sum(1 for ch in word if ord(ch) >= 0x80)
    ascii_chars = len(word) - cjk
    return cjk <= MAX_CJK_CHARS and ascii_chars <= MAX_ASCII_CHARS


def score(stat: HotwordStat, now: float | None = None) -> float:
    now = time.time() if now is None else now
    age = max(0.0, now - stat.last_used_ts) if stat.last_used_ts > 0 else float("inf")
    recency = math.exp(-age / RECENCY_TAU_S) if math.isfinite(age) else 0.0
    manual = 1.0 if stat.source == "manual" else 0.0
    return (W_COUNT * math.log1p(max(0, stat.count))
            + W_RECENCY * recency
            + W_MANUAL * manual)


def rank(stats: list[HotwordStat], now: float | None = None) -> list[str]:
    """按评分降序输出有效热词（过滤非法词），平分按字典序保证确定性。"""
    valid = [s for s in stats if is_valid_word(s.word)]
    valid.sort(key=lambda s: (-score(s, now), s.word))
    return [s.word for s in valid]


def fit_token_budget(words: list[str], budget: int = VOLC_STREAM_TOKEN_BUDGET,
                     max_word_tokens: int = VOLC_STREAM_MAX_WORD_TOKENS) -> list[str]:
    """火山流式直传裁剪：按给定顺序逐个装入，单词超限或累计超预算的丢弃。"""
    fitted: list[str] = []
    used = 0
    for word in words:
        tokens = estimate_tokens(word)
        if tokens > max_word_tokens or used + tokens > budget:
            continue
        fitted.append(word)
        used += tokens
    return fitted


def fit_count_limit(words: list[str], limit: int) -> list[str]:
    """腾讯临时表（128 词）/表通道（1000/5000 词）裁剪：截前 limit 个。"""
    return words[:limit]


def layered_plan(stats: list[HotwordStat], now: float | None = None) -> dict:
    """三层通道分配：高频层会话级直传，中频层平台热词表，长尾层 LLM 精修兜底。

    返回 {"direct": [...], "tencent_temp": [...], "table": [...], "longtail": [...]}：
    direct       = 火山流式 80 tokens 预算内的高频词（会话级 context 直传）
    tencent_temp = 腾讯临时热词表 128 词（与 direct 同源，上限不同）
    table        = 直传装不下、进表通道的词（≤5000，火山/腾讯表共用此层）
    longtail     = 表也装不下的长尾，只进 LLM 精修 prompt
    """
    ordered = rank(stats, now)
    direct = fit_token_budget(ordered)
    direct_set = set(direct)
    rest = [w for w in ordered if w not in direct_set]
    return {
        "direct": direct,
        "tencent_temp": fit_count_limit(ordered, TENCENT_TEMP_LIMIT),
        "table": fit_count_limit(rest, VOLC_TABLE_LIMIT),
        "longtail": rest[VOLC_TABLE_LIMIT:],
    }


def load_stats_json(path: str) -> list[HotwordStat]:
    """从 JSON 加载热词统计。接受两种形态：
    [{"word": ..., "count": ..., "last_used_ts": ..., "source": ...}, ...]
    或 {word: count, ...}（hotword_candidates.json 简化形态，无时间戳/来源）。
    """
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    if isinstance(data, dict):
        return [HotwordStat(word=w, count=int(c)) for w, c in data.items()]
    return [HotwordStat(word=item["word"], count=int(item.get("count", 0)),
                        last_used_ts=float(item.get("last_used_ts", 0.0)),
                        source=item.get("source", "mined"))
            for item in data]


def _self_test() -> None:
    now = time.time()
    stats = [
        HotwordStat(word="Opus", count=3, last_used_ts=now, source="mined"),
        HotwordStat(word="VoiceStick", count=3, last_used_ts=now - 86400, source="manual"),
        HotwordStat(word="覃海洋", count=1, last_used_ts=now - 90 * 86400, source="mined"),
        HotwordStat(word="BLE", count=30, last_used_ts=now - 3600, source="mined"),
        HotwordStat(word="带空格 的词", count=99, last_used_ts=now, source="manual"),  # 非法
        HotwordStat(word="超过十个汉字的热词词汇总长度", count=99, source="manual"),  # 11 字非法
    ]
    ordered = rank(stats, now)
    assert "带空格 的词" not in ordered and "超过十个汉字的热词词汇总长度" not in ordered
    # 同计数下手动词（2.0 加权）应胜过挖掘词
    assert ordered.index("VoiceStick") < ordered.index("Opus"), ordered
    # 但高频词（log1p(30)≈3.4）能胜过低计数手动词——频率优先的设计意图
    high_freq = rank([HotwordStat(word="高频词", count=30, last_used_ts=now),
                      HotwordStat(word="冷门手动词", count=0, last_used_ts=now,
                                  source="manual")], now)
    assert high_freq[0] == "高频词", high_freq
    assert estimate_tokens("Opus") == 2 and estimate_tokens("覃海洋") == 3
    assert not is_valid_word("") and not is_valid_word("a b")
    # 预算裁剪：塞入大量词直到超 80 tokens
    many = [HotwordStat(word=f"术语{i:03d}", count=100 - i) for i in range(100)]
    plan = layered_plan(many, now)
    total_tokens = sum(estimate_tokens(w) for w in plan["direct"])
    assert total_tokens <= VOLC_STREAM_TOKEN_BUDGET, total_tokens
    assert len(plan["tencent_temp"]) == 100  # 未超 128
    assert len(plan["table"]) + len(plan["direct"]) == 100
    print(f"OK rank={ordered}")
    print(f"OK direct={len(plan['direct'])} 词 {total_tokens} tokens, "
          f"table={len(plan['table'])}, longtail={len(plan['longtail'])}")


if __name__ == "__main__":
    _self_test()
