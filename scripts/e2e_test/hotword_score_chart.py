#!/usr/bin/env python3
"""热词活跃度评分柱状图：把 hotword_usage.json 的活跃度评分画成 PNG。

评分公式与桌面端 hotword_selector.cc / asr_bench/hotword_select.py 完全一致：
    score = 1.0 * log1p(count) + 0.5 * exp(-age / 30d) + 2.0 * (source == "manual")

柱按分数降序排列（第一个最高、依次下降），manual / mined 两种来源用不同颜色，
便于直观看出「为什么第一个热词度很高、第二个又降下来」（使用次数 + 新近度 +
手动加成三因素叠加的结果）。

自测：python hotword_score_chart.py [--stats-file PATH] [--out PATH.png]
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from asr_bench import hotword_select  # noqa: E402

# 图尺寸与排版常量
DPI = 150
TOP_N = 40           # 词数过多时只画前 40 个（分数最高的），避免横轴挤死
FIG_W_PER_BAR = 0.45  # 每根柱子分配的横向宽度（英寸）
FIG_H = 5.5
BAR_LABEL_FONTSIZE = 8
WORD_LABEL_FONTSIZE = 10

COLOR_MANUAL = "#4C9BD6"  # 手动加词（+2.0 加成）
COLOR_MINED = "#F0A04B"   # 候选挖掘


def default_usage_path() -> Path | None:
    """桌面端热词统计文件的默认位置（与 config.toml 同目录）。"""
    if sys.platform == "darwin":
        p = Path.home() / "Library" / "Application Support" / "VoiceStick" / "hotword_usage.json"
    else:
        appdata = os.environ.get("APPDATA")
        if not appdata:
            return None
        p = Path(appdata) / "VoiceStick" / "hotword_usage.json"
    return p if p.exists() else None


def load_stats(stats_path: Path) -> list[hotword_select.HotwordStat]:
    """从 hotword_usage.json 加载全部热词统计（兼容 load_stats_json 的两种形态）。"""
    return hotword_select.load_stats_json(str(stats_path))


def render_chart(stats: list[hotword_select.HotwordStat], out_path: Path,
                 now: float | None = None, title_suffix: str = "") -> Path:
    """画活跃度评分柱状图并保存 PNG。返回输出路径。

    stats 顺序任意，内部按 score 降序重排（同分按 count 降序、字典序，与
    桌面端 BuildHotwordTray 的排序规则一致）。
    """
    import matplotlib

    matplotlib.use("Agg")  # 无界面后端，回归测试在 CI / 后台也能跑
    import matplotlib.pyplot as plt

    # Windows 上 matplotlib 默认字体不含中文，需显式指定（词名都是中文）。
    plt.rcParams["font.sans-serif"] = ["Microsoft YaHei", "SimHei",
                                       "PingFang SC", "sans-serif"]
    plt.rcParams["axes.unicode_minus"] = False

    now = time.time() if now is None else now
    valid = [s for s in stats if hotword_select.is_valid_word(s.word)]
    valid.sort(key=lambda s: (-hotword_select.score(s, now),
                              -s.count, s.word))
    truncated = len(valid) > TOP_N
    shown = valid[:TOP_N]

    words = [s.word for s in shown]
    scores = [hotword_select.score(s, now) for s in shown]
    colors = [COLOR_MANUAL if s.source == "manual" else COLOR_MINED
              for s in shown]

    width = max(8.0, len(shown) * FIG_W_PER_BAR)
    fig, ax = plt.subplots(figsize=(width, FIG_H))
    bars = ax.bar(range(len(shown)), scores, color=colors, width=0.7)
    for bar, sc in zip(bars, scores):
        ax.text(bar.get_x() + bar.get_width() / 2, bar.get_height(),
                f"{sc:.2f}", ha="center", va="bottom",
                fontsize=BAR_LABEL_FONTSIZE)
    ax.set_xticks(range(len(shown)))
    ax.set_xticklabels(words, rotation=45, ha="right",
                       fontsize=WORD_LABEL_FONTSIZE)
    ax.set_ylabel("活跃度评分")
    title = "热词活跃度评分（log1p(count) + 新近度衰减 + manual 加成）"
    if title_suffix:
        title += f" — {title_suffix}"
    ax.set_title(title)
    ax.grid(axis="y", linestyle="--", alpha=0.4)
    ax.set_ylim(0, max(scores) * 1.15 if scores else 1.0)
    import matplotlib.patches as mpatches

    ax.legend(handles=[
        mpatches.Patch(color=COLOR_MANUAL, label="manual（手动加词 +2.0）"),
        mpatches.Patch(color=COLOR_MINED, label="mined（候选挖掘）"),
    ])

    out_path.parent.mkdir(parents=True, exist_ok=True)
    fig.tight_layout()
    fig.savefig(out_path, dpi=DPI)
    plt.close(fig)
    if truncated:
        print(f"WARN: 热词 {len(valid)} 个超过 TOP_N={TOP_N}，"
              f"图只展示分数最高的前 {TOP_N} 个", file=sys.stderr)
    return out_path


def render_chart_from_file(stats_path: Path, out_path: Path,
                           title_suffix: str = "") -> Path:
    stats = load_stats(stats_path)
    if not stats:
        raise ValueError(f"热词统计为空：{stats_path}")
    return render_chart(stats, out_path, title_suffix=title_suffix)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--stats-file", default="",
                    help="hotword_usage.json 路径（默认自动探测桌面端位置）")
    ap.add_argument("--out", default="", help="PNG 输出路径（默认 stdout 旁）")
    args = ap.parse_args()
    stats_path = (Path(args.stats_file) if args.stats_file
                  else default_usage_path())
    if stats_path is None:
        print("未找到 hotword_usage.json（--stats-file 显式指定，"
              "或先运行桌面端产生统计）", file=sys.stderr)
        return 2
    if not stats_path.exists():
        print(f"统计文件不存在：{stats_path}", file=sys.stderr)
        return 2
    stamp = time.strftime("%Y%m%d-%H%M%S")
    out_path = (Path(args.out) if args.out
                else HERE / "bench_results" / f"hotword_score_{stamp}.png")
    path = render_chart_from_file(stats_path, out_path)
    print(f"OK {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
