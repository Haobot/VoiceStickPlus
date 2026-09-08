"""评测指标：CER / RTF / 热词命中错误率 / 偏置降幅。

口径说明（M0 验收材料的一部分）：
- CER：先做文本归一化（全角转半角、去标点空白、大小写折叠），再按字符编辑距离计算，
  与 jiwer 对中文的习惯口径一致；本模块自实现以保证归一化规则可测试、可复现。
- 热词命中：归一化后整词子串匹配（大小写/标点不敏感，但词形必须完整）。
- 热词命中错误率：未命中实例数 / 总实例数（每句热词出现一次共 20 实例）。
"""
from __future__ import annotations

import unicodedata
from dataclasses import dataclass

# 中文标点 + 常见英文标点之外的字符类别判定用 Unicode 类别，无需手工枚举全集


def normalize_for_cer(text: str) -> str:
    """CER 归一化：全角→半角、去标点/空白、小写。"""
    # NFKC 把全角字母数字、兼容字符折叠为半角
    folded = unicodedata.normalize("NFKC", text)
    kept = [ch.lower() for ch in folded if ch.isalnum()]
    # 中文没有大小写问题；isalnum 同时保留汉字、字母、数字
    return "".join(kept)


def normalize_for_match(text: str) -> str:
    """热词命中判定的归一化，与 CER 同口径。"""
    return normalize_for_cer(text)


def _edit_distance(ref: str, hyp: str) -> int:
    """Levenshtein 编辑距离（字符级），双行滚动数组实现。"""
    if not ref:
        return len(hyp)
    if not hyp:
        return len(ref)
    prev = list(range(len(hyp) + 1))
    for i, rch in enumerate(ref, start=1):
        cur = [i] + [0] * len(hyp)
        for j, hch in enumerate(hyp, start=1):
            cost = 0 if rch == hch else 1
            cur[j] = min(prev[j] + 1,        # 删除
                         cur[j - 1] + 1,     # 插入
                         prev[j - 1] + cost) # 替换
        prev = cur
    return prev[-1]


def cer(reference: str, hypothesis: str) -> float:
    """字符错误率 = 编辑距离(归一化后) / 参考长度。参考为空时：皆空 0，否则 1。"""
    ref = normalize_for_cer(reference)
    hyp = normalize_for_cer(hypothesis)
    if not ref:
        return 0.0 if not hyp else 1.0
    return _edit_distance(ref, hyp) / len(ref)


def rtf(elapsed_seconds: float, audio_seconds: float) -> float:
    """实时系数 = 处理耗时 / 音频时长。非正输入一律返回 0.0 防御除零。"""
    if elapsed_seconds <= 0 or audio_seconds <= 0:
        return 0.0
    return elapsed_seconds / audio_seconds


def hotword_hit(hypothesis: str, hotword: str) -> bool:
    """热词是否在识别输出中命中（归一化子串，词形必须完整）。"""
    return normalize_for_match(hotword) in normalize_for_match(hypothesis)


@dataclass(frozen=True)
class HotwordStats:
    """热词命中统计结果。"""

    total: int
    hits: int

    @property
    def misses(self) -> int:
        return self.total - self.hits

    @property
    def error_rate(self) -> float:
        if self.total == 0:
            return 0.0
        return self.misses / self.total


def hotword_error_stats(samples: list[tuple[str, str]]) -> HotwordStats:
    """统计 (识别输出, 目标热词) 样本列表的命中情况。"""
    hits = sum(1 for hyp, word in samples if hotword_hit(hyp, word))
    return HotwordStats(total=len(samples), hits=hits)


def reduction_rate(baseline: float, biased: float) -> float:
    """错误率降幅 = (基线 - 偏置) / 基线。基线为 0 时无意义，返回 0.0。"""
    if baseline <= 0:
        return 0.0
    return (baseline - biased) / baseline
