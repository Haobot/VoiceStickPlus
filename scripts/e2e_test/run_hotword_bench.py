#!/usr/bin/env python3
"""热词专项评测：大热词库下腾讯/火山 ASR 的热词识别效果对比。

设计见 Doc/Plan/hotword-eval-and-prioritization.md。对热词语料
（corpus/hotword_corpus.json，每条标注目标热词）+ 对照语料
（corpus/corpus.json，无目标热词，用于误触发统计）按配置矩阵回放，
量化每组的热词命中率 / CER / 误触发率 / 延迟。

配置矩阵：
  baseline     不带任何热词（基线）
  direct_small 只直传语料目标热词（理想小库，火山 80 tokens / 腾讯临时表）
  direct_full  全库按字典序塞满会话预算（无脑塞满对照组）
  tiered       全库按频率评分裁剪（hotword_select 策略组，--library-size 控制库规模）
  table        平台热词表通道（需 config.toml 或命令行提供表 ID，缺失则 SKIP）
  no_nonstream 火山关闭二遍 + 目标热词直传（验证热词只对第一遍生效）

用法：
    python run_hotword_bench.py --provider all --configs baseline,direct_small --rounds 1
    python run_hotword_bench.py --provider volcengine --configs tiered --library-size 500

凭据从 %APPDATA%/VoiceStick/config.toml 读取。退出码 0=运行完成；2=凭据/语料缺失。
"""
from __future__ import annotations

import argparse
import json
import random
import re
import sys
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from asr_bench import hotword_select, tencent, volcengine  # noqa: E402
from asr_bench.metrics import cer, hotword_report, summarize  # noqa: E402
from asr_bench.result import ClipResult  # noqa: E402
from asr_bench.wsproto import load_windows_config  # noqa: E402

CORPUS_DIR = HERE / "corpus"
DEFAULT_OUT = HERE / "bench_results"

ALL_CONFIGS = ["baseline", "direct_small", "direct_full", "tiered", "table",
               "no_nonstream"]

# 词库填充词：模拟用户真实热词库里的长尾词（产品/框架/术语），凑库规模用。
FILLER_WORDS = [
    "DeepSeek", "Kubernetes", "Docker", "Redis", "Kafka", "Flink", "Rust",
    "Golang", "TypeScript", "Vite", "Vue", "React", "Electron", "Qt",
    "CMake", "Ninja", "Gradle", "Maven", "Jenkins", "GitLab", "Nginx",
    "PostgreSQL", "MySQL", "MongoDB", "SQLite", "ClickHouse", "Elasticsearch",
    "Prometheus", "Grafana", "TensorFlow", "PyTorch", "ONNX", "CUDA",
    "鸿蒙", "麒麟", "飞书", "钉钉", "企业微信", "石墨文档", "语雀",
    "Notion", "Figma", "Linear", "Jira", "Confluence", "Slack", "Zoom",
    "豆包", "通义千问", "文心一言", "混元", "星火", "Kimi", "Claude",
    "Copilot", "Cursor", "Windsurf", "Trae", "CodeBuddy", "Aider",
    "transformer", "注意力机制", "大语言模型", "多模态", "强化学习", "蒸馏",
    "量化交易", "供应链金融", "碳中和", "新质生产力", "专精特新", "跨境电商",
    "自动驾驶", "激光雷达", "毫米波", "域控制器", "线控底盘", "碳化硅",
    "基因测序", "蛋白质折叠", "靶向药", "免疫治疗", "核磁共振", "内窥镜",
]


def load_manifest(path: Path, only: set[str] | None) -> list[dict]:
    if not path.exists():
        return []
    manifest = json.loads(path.read_text(encoding="utf-8"))
    items = []
    for item in manifest:
        if only and item["id"] not in only:
            continue
        ogg = path.parent / f"{item['id']}.ogg"
        pcm = path.parent / f"{item['id']}.pcm"
        if not ogg.exists() or not pcm.exists():
            print(f"WARN: {item['id']} 缺少 ogg/pcm，跳过（先跑 gen_corpus.py "
                  f"--manifest {path.name}）", file=sys.stderr)
            continue
        item["_ogg"] = ogg
        item["_duration"] = pcm.stat().st_size / 32000.0
        items.append(item)
    return items


def build_library(target_words: list[str], size: int) -> list[str]:
    """目标热词 + 填充词凑到指定库规模（去重，保持目标词在前）。"""
    library = list(dict.fromkeys(target_words))
    fillers = [w for w in FILLER_WORDS if w not in library]
    i = 0
    while len(library) < size:
        if i < len(fillers):
            library.append(fillers[i])
        else:
            library.append(f"术语{i:04d}")
        i += 1
    return library


def build_stats(library: list[str], target_words: list[str]) -> list[hotword_select.HotwordStat]:
    """构造确定性使用统计：目标词高频（模拟真实高频热词），填充词长尾。"""
    rng = random.Random(42)
    targets = set(target_words)
    stats = []
    for word in library:
        if word in targets:
            stats.append(hotword_select.HotwordStat(
                word=word, count=rng.randint(20, 100), last_used_ts=1.0))
        else:
            stats.append(hotword_select.HotwordStat(
                word=word, count=rng.randint(0, 3), last_used_ts=1.0))
    return stats


# 腾讯热词（临时表与词表 API 共同约束）：不能含空格，且实测词表 API 拒绝
# 含 '.' 的词（InvalidParameterValue.InvalidWordWeight）。保守起见只放行
# 中英文/数字/连字符/下划线，其余词降级到 LLM 精修兜底（桌面端
# SyncHotwords 目前未做此过滤，热词含 '.' 会同步失败——已记录为桌面端待修项）。
TENCENT_WORD_RE = re.compile(r"^[0-9A-Za-z一-鿿_-]+$")


def tencent_hotword_list(words: list[str], weight: int = 5) -> str:
    """腾讯临时热词表格式："词|权重" 逗号分隔，≤128 词，词内不能含 | 和逗号。"""
    clean = [w for w in words if "|" not in w and "," not in w
             and TENCENT_WORD_RE.match(w)]
    return ",".join(f"{w}|{weight}" for w in clean[:hotword_select.TENCENT_TEMP_LIMIT])


def config_kwargs(config: str, provider: str, *, target_words: list[str],
                  library: list[str], stats: list[hotword_select.HotwordStat],
                  cfg: dict[str, str], args) -> dict | None:
    """每组配置映射到 run_clip 的关键字参数；None 表示该配置对平台不适用/缺凭据。"""
    if config == "baseline":
        return {}
    if config == "direct_small":
        if provider == "volcengine":
            return {"hotwords": hotword_select.fit_token_budget(target_words)}
        return {"hotword_list": tencent_hotword_list(target_words, weight=10)}
    if config == "direct_full":
        # 无脑塞满：全库按字典序装入预算（与频率无关的对照组）
        alpha = sorted(library)
        if provider == "volcengine":
            return {"hotwords": hotword_select.fit_token_budget(alpha)}
        return {"hotword_list": tencent_hotword_list(alpha)}
    if config == "tiered":
        plan = hotword_select.layered_plan(stats, now=1.0)
        if provider == "volcengine":
            return {"hotwords": plan["direct"]}
        return {"hotword_list": tencent_hotword_list(plan["tencent_temp"])}
    if config == "table":
        if provider == "volcengine":
            table_id = args.boosting_table_id or cfg.get("volcengine_boosting_table_id", "")
            if not table_id:
                return None
            return {"boosting_table_id": table_id}
        table_id = args.tencent_hotword_id or cfg.get("tencent_hotword_id", "")
        if not table_id:
            return None
        return {"hotword_id": table_id}
    if config == "no_nonstream":
        if provider != "volcengine":
            return None
        return {"enable_nonstream": False,
                "hotwords": hotword_select.fit_token_budget(target_words)}
    raise ValueError(f"unknown config {config}")


def run_one(provider: str, item: dict, round_no: int, cfg: dict[str, str],
            timeout: float, extra: dict) -> ClipResult:
    common = dict(duration_s=item["_duration"], clip_id=item["id"], round_no=round_no,
                  category=item.get("category", ""), reference=item["text"])
    if provider == "volcengine":
        return volcengine.run_clip(item["_ogg"], api_key=cfg["volcengine_api_key"],
                                   timeout=timeout, **common, **extra)
    return tencent.run_clip(item["_ogg"], secret_id=cfg["tencent_secret_id"],
                            secret_key=cfg["tencent_secret_key"],
                            appid=cfg["tencent_appid"], timeout=timeout, **common, **extra)


def write_hotword_reports(report: dict, out_dir: Path, stamp: str) -> tuple[Path, Path]:
    out_dir.mkdir(parents=True, exist_ok=True)
    json_path = out_dir / f"hotword_{stamp}.json"
    md_path = out_dir / f"hotword_{stamp}.md"
    json_path.write_text(json.dumps(report, ensure_ascii=False, indent=2),
                         encoding="utf-8")
    lines = [
        f"# 热词评测报告 {stamp}",
        "",
        f"- 热词语料：{report['hotword_clips']} 条；对照语料：{report['control_clips']} 条",
        f"- 热词库规模：{report['library_size']} 词（目标热词 {len(report['target_words'])} 个）",
        f"- 轮次：{report['rounds']}",
        "",
        "| 配置 | 平台 | 成功率 | 热词命中率 | CER | 误触发率 | 尾延迟 p50 |",
        "|---|---|---|---|---|---|---|",
    ]
    for config, providers in report["configs"].items():
        for provider, block in providers.items():
            hw = block["hotwords"]["overall"]
            s = block["summary"]
            ft = block["hotwords"]["false_trigger"]
            tail = s["tail_latency_ms"]["p50"]
            lines.append(
                f"| {config} | {provider} | {s['success']}/{s['runs']} "
                f"| {hw['hit']}/{hw['total']} ({hw['recall']}) "
                f"| {s['cer_mean']} | {ft['rate']} "
                f"| {f'{tail:.0f}ms' if isinstance(tail, (int, float)) else '—'} |")
    lines += ["", "## 逐词命中率", ""]
    for config, providers in report["configs"].items():
        for provider, block in providers.items():
            lines.append(f"### {config} / {provider}")
            lines.append("")
            for word, s in block["hotwords"]["per_word"].items():
                lines.append(f"- {word}: {s['hit']}/{s['total']} ({s['recall']})")
            lines.append("")
    md_path.write_text("\n".join(lines), encoding="utf-8")
    return json_path, md_path


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except Exception:  # noqa: BLE001
        pass
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--provider", choices=["all", "volcengine", "tencent"], default="all")
    ap.add_argument("--configs", default="baseline,direct_small",
                    help="逗号分隔配置组，或 all（默认 baseline,direct_small 冒烟）")
    ap.add_argument("--rounds", type=int, default=1)
    ap.add_argument("--library-size", type=int, default=500,
                    help="tiered/direct_full 组的热词库总规模（默认 500）")
    ap.add_argument("--clips", default="", help="只跑指定热词语料，逗号分隔 id")
    ap.add_argument("--no-control", action="store_true", help="不跑对照语料")
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--boosting-table-id", default="", help="火山热词表 ID（覆盖 config.toml）")
    ap.add_argument("--tencent-hotword-id", default="", help="腾讯热词表 ID（覆盖 config.toml）")
    ap.add_argument("--create-tables", action="store_true",
                    help="table 组缺表 ID 时自动把词库同步到腾讯热词表"
                         "（VoiceStick-Bench-Hotwords，存在则更新），会在账号下创建/改写表")
    ap.add_argument("--out", default=str(DEFAULT_OUT))
    ap.add_argument("--stamp", default=datetime.now().strftime("%Y%m%d-%H%M%S"))
    args = ap.parse_args()

    cfg = load_windows_config()
    providers = ["volcengine", "tencent"] if args.provider == "all" else [args.provider]
    configs = ALL_CONFIGS if args.configs == "all" else \
        [c.strip() for c in args.configs.split(",") if c.strip()]
    for c in configs:
        if c not in ALL_CONFIGS:
            print(f"未知配置组: {c}（可选 {','.join(ALL_CONFIGS)} 或 all）", file=sys.stderr)
            return 2
    missing = []
    if "volcengine" in providers and not cfg.get("volcengine_api_key"):
        missing.append("volcengine_api_key")
    if "tencent" in providers:
        for k in ("tencent_secret_id", "tencent_secret_key", "tencent_appid"):
            if not cfg.get(k):
                missing.append(k)
    if missing:
        print(f"缺少凭据: {', '.join(missing)}（config.toml）", file=sys.stderr)
        return 2

    only = {c.strip() for c in args.clips.split(",") if c.strip()} or None
    hotword_clips = load_manifest(CORPUS_DIR / "hotword_corpus.json", only)
    if not hotword_clips:
        print("热词语料为空（先跑 gen_corpus.py --manifest corpus/hotword_corpus.json）",
              file=sys.stderr)
        return 2
    control_clips = [] if args.no_control else load_manifest(CORPUS_DIR / "corpus.json", None)
    clips = hotword_clips + control_clips
    clip_hotwords = {item["id"]: item.get("hotwords", []) for item in hotword_clips}
    target_words = sorted({w for item in hotword_clips for w in item.get("hotwords", [])})

    library = build_library(target_words, max(args.library_size, len(target_words)))
    stats = build_stats(library, target_words)

    # table 组：缺腾讯表 ID 且指定 --create-tables 时，把词库同步进评测专用热词表
    if ("table" in configs and "tencent" in providers
            and not args.tencent_hotword_id and not cfg.get("tencent_hotword_id")
            and args.create_tables):
        from asr_bench import tencent_vocab
        ranked = [w for w in hotword_select.rank(stats, now=1.0)
                  if TENCENT_WORD_RE.match(w)]
        dropped = len(hotword_select.rank(stats, now=1.0)) - len(ranked)
        args.tencent_hotword_id = tencent_vocab.sync_vocab(
            "VoiceStick-Bench-Hotwords", ranked[:1000],
            secret_id=cfg["tencent_secret_id"], secret_key=cfg["tencent_secret_key"],
            description="热词评测自动同步，可删除")
        print(f"腾讯热词表已同步: {args.tencent_hotword_id} "
              f"({min(len(ranked), 1000)} 词，{dropped} 词因字符限制未入表)")

    report: dict = {
        "stamp": args.stamp,
        "rounds": args.rounds,
        "library_size": len(library),
        "target_words": target_words,
        "hotword_clips": len(hotword_clips),
        "control_clips": len(control_clips),
        "configs": {},
    }

    for config in configs:
        for provider in providers:
            extra = config_kwargs(config, provider, target_words=target_words,
                                  library=library, stats=stats, cfg=cfg, args=args)
            if extra is None:
                print(f"SKIP {config}/{provider}（平台不适用或缺表 ID）")
                continue
            results: list[ClipResult] = []
            total = args.rounds * len(clips)
            done = 0
            for round_no in range(1, args.rounds + 1):
                for item in clips:
                    done += 1
                    res = run_one(provider, item, round_no, cfg, args.timeout, extra)
                    results.append(res)
                    tag = "OK " if res.success else "FAIL"
                    note = (f" cer={cer(res.reference, res.final_text):.3f}"
                            if res.success else f" err={res.error}")
                    print(f"[{config}/{provider} {done}/{total}] {tag} "
                          f"r{round_no} {item['id']}{note}", flush=True)
            report["configs"].setdefault(config, {})[provider] = {
                "params": {k: (v if isinstance(v, (str, int, bool)) else f"<{len(v)} 词>")
                           for k, v in extra.items()},
                "summary": summarize(results),
                "hotwords": hotword_report(results, clip_hotwords, library),
                "detail": [r.to_dict() for r in results],
            }
            hw = report["configs"][config][provider]["hotwords"]["overall"]
            print(f"== {config}/{provider}: 热词命中 {hw['hit']}/{hw['total']} "
                  f"({hw['recall']})", flush=True)
            # 每组完成即落盘部分结果：长时间网络评测中途崩溃/断网不丢已完成组
            partial_path = Path(args.out) / f"hotword_{args.stamp}_partial.json"
            partial_path.parent.mkdir(parents=True, exist_ok=True)
            partial_path.write_text(json.dumps(report, ensure_ascii=False, indent=2),
                                    encoding="utf-8")

    if not report["configs"]:
        print("无任何可运行配置", file=sys.stderr)
        return 1
    json_path, md_path = write_hotword_reports(report, Path(args.out), args.stamp)
    print(f"\nJSON: {json_path}")
    print(f"MD:   {md_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
