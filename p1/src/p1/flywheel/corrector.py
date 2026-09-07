"""热词后处理纠正器：SenseVoice 无解码偏置，识别后按热词库替换错词。

匹配模型（alias 统一）：热词 surface 与所有 pron 变体都是「别名」，
任何别名命中即替换为 surface 原形。别名分两类各走相似度通道：
- 拉丁别名（纯 ASCII）：对文本中的英文/数字片段做 casefold 精确 → 编辑距离 fuzzy；
- 中文别名（含汉字）：对中文片段滑窗，拼音序列 fuzzy（懒拼音，忽略声调）。

误伤控制：fuzzy 阈值 0.85 + 片段与别名长度差 ≤ 3 双门限（M0 评测
kuernetes→Kubernetes 类单字符错误 ratio ≈ 95，正常远高于阈值）。
"""
from __future__ import annotations

import re
from dataclasses import dataclass

from pypinyin import lazy_pinyin
from rapidfuzz import fuzz

from p1.flywheel.hotword_store import HotwordEntry

FUZZY_THRESHOLD = 85.0
MAX_LEN_GAP = 3
_LATIN_TOKEN = re.compile(r"[A-Za-z0-9][A-Za-z0-9+.#-]*")
_CJK_TOKEN = re.compile(r"[\u4e00-\u9fff]+")
_HAS_CJK = re.compile(r"[\u4e00-\u9fff]")


@dataclass(frozen=True)
class CorrectionEvent:
    """一次热词纠正（悬浮条反馈与 record_hit 飞轮回调的依据）。"""

    wrong: str
    right: str
    entry_id: str


@dataclass(frozen=True)
class _Alias:
    alias: str
    surface: str
    entry_id: str
    pinyin: str  # 中文别名的拼音串；拉丁别名与 alias 相同


def _pinyin_key(text: str) -> str:
    return "".join(lazy_pinyin(text))


class HotwordCorrector:
    """识别文本 → 热词纠正文本 + 纠正事件列表。"""

    def __init__(self, entries: list[HotwordEntry]):
        self._latin: dict[str, _Alias] = {}   # casefold 别名 → 替换目标
        self._latin_fuzzy: list[_Alias] = []
        self._cjk: list[_Alias] = []
        for entry in entries:
            for alias in [entry.surface, *entry.pron]:
                self._register(alias, entry)

    def _register(self, alias: str, entry: HotwordEntry) -> None:
        alias = alias.strip()
        if not alias or alias == entry.surface and not alias:
            return
        item = _Alias(alias=alias, surface=entry.surface,
                      entry_id=entry.id,
                      pinyin=_pinyin_key(alias) if _HAS_CJK.search(alias) else alias)
        if _HAS_CJK.search(alias):
            self._cjk.append(item)
        else:
            self._latin[alias.casefold()] = item
            self._latin_fuzzy.append(item)

    # ---- 对外主入口 ----

    def correct(self, text: str) -> tuple[str, list[CorrectionEvent]]:
        """返回（纠正后文本, 纠正事件列表）；原文本不做原地修改。"""
        events: list[CorrectionEvent] = []
        spans: list[tuple[int, int, _Alias]] = []

        for m in _LATIN_TOKEN.finditer(text):
            hit = self._match_latin(m.group())
            if hit:
                spans.append((m.start(), m.end(), hit))

        for m in _CJK_TOKEN.finditer(text):
            for start, end, hit in self._match_cjk(m.group(), m.start()):
                spans.append((start, end, hit))

        # 去重叠（保留先出现的），从后往前替换避免 span 偏移失效
        spans.sort(key=lambda s: s[0])
        picked: list[tuple[int, int, _Alias]] = []
        last_end = -1
        for span in spans:
            if span[0] >= last_end:
                picked.append(span)
                last_end = span[1]
        result = text
        for start, end, hit in reversed(picked):
            result = result[:start] + hit.surface + result[end:]
            events.append(CorrectionEvent(
                wrong=text[start:end], right=hit.surface, entry_id=hit.entry_id))
        events.reverse()
        return result, events

    # ---- 匹配通道 ----

    def _match_latin(self, token: str) -> _Alias | None:
        exact = self._latin.get(token.casefold())
        if exact and exact.surface != token:
            return exact  # 大小写形态纠正（含别名替换）
        if exact:
            return None  # 已是正确原形
        folded = token.casefold()
        for cand in self._latin_fuzzy:
            if abs(len(cand.alias) - len(token)) > MAX_LEN_GAP:
                continue
            # rapidfuzz 大小写敏感，先折叠再比较（kuernetes→Kubernetes 靠这里）
            if fuzz.ratio(cand.alias.casefold(), folded) >= FUZZY_THRESHOLD:
                return cand
        return None

    def _match_cjk(self, segment: str, base: int) -> list[tuple[int, int, _Alias]]:
        """中文段滑窗拼音匹配；每个别名只取相似度最高的窗口。

        取最优而非首个过线窗口，避免「在库伯内提斯」这类窗口把
        前缀邻字一起吞掉（ratio 91.7 的窗口输给 ratio 100 的精确子串）。
        """
        hits: list[tuple[int, int, _Alias]] = []
        for cand in self._cjk:
            best: tuple[float, int, int] | None = None
            window_len = len(cand.alias)
            for size in range(max(1, window_len - 2), window_len + 3):
                for i in range(0, len(segment) - size + 1):
                    score = fuzz.ratio(cand.pinyin, _pinyin_key(segment[i:i + size]))
                    if score < FUZZY_THRESHOLD:
                        continue
                    if best is None or score > best[0]:
                        best = (score, base + i, base + i + size)
            if best:
                hits.append((best[1], best[2], cand))
        return hits


def build_corrector_from_entries(entries: list[HotwordEntry]) -> HotwordCorrector:
    """工厂（Pipeline 组装入口，保留扩展点：未来可按类别过滤候选）。"""
    return HotwordCorrector(entries)
