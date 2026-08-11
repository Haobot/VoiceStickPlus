#!/usr/bin/env python3
"""热词识别自动化验收测试：当前热词配置下，热词能不能被真实 ASR 识别到。

与 `run_hotword_bench.py`（策略矩阵对比）不同，本工具做的是「桌面端同款发送」
的验收回归：把配置里的热词按桌面端完全一致的逻辑发给真实 ASR（腾讯=词表通道
hotword_id / 火山=corpus.context 评分裁剪直传），回放音频，报告每个热词的
命中率与识别原文，并给出「为什么没识别到」的诊断（含不可入表的词）。

音频来源二选一：
  1. 自动生成（默认）：为每个热词自动造句（中英模板轮换）并 edge-tts 合成
     （复用 gen_corpus.gen_one），产物在 corpus/acceptance_<stamp>/，不入库。
  2. 已有语音：--manifest 指定标注清单
     [{"id","text","hotwords","audio":"path/to.ogg"}, ...]，
     audio 可指向真实调试录音（%APPDATA%\\VoiceStick\\*.ogg），
     text=参考文本（CER 用）、hotwords=该句目标词（命中 ground truth）。

发送组：
  baseline  不带任何热词（基线，度量「不靠热词本来就识对」的比例）
  desktop   桌面端同款发送：
             腾讯  SyncHotwords 同款：字符过滤（空格/'.' 拒收）后同步词表 →
                   hotword_id；被过滤词记入 unsendable（唯一兜底=LLM 精修）
             火山  RankedHotwordsForAsr 同款：评分排序（可选 --stats-file
                   用真实 hotword_usage.json，缺省按全 manual）装入 80 tokens

凭据从 %APPDATA%/VoiceStick/config.toml 读取（与其余回放工具一致，不打印）。
退出码 0=运行完成（命中率高低不影响退出码，本工具只负责测量）；2=凭据/依赖缺失。
"""
from __future__ import annotations

import argparse
import asyncio
import json
import re
import sys
import tempfile
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import gen_corpus  # noqa: E402
from asr_bench import hotword_select, tencent, tencent_vocab, volcengine  # noqa: E402
from asr_bench.metrics import cer, hotword_hit, hotword_report, summarize  # noqa: E402
from asr_bench.result import ClipResult  # noqa: E402
from asr_bench.wsproto import load_windows_config  # noqa: E402

DEFAULT_OUT = HERE / "bench_results"

# 与桌面端 TencentAsrVocabClient::IsValidHotwordChars 一致：只放行中英文/数字/_-，
# 含空格、'.' 及其余 ASCII 标点的词进不了腾讯任何热词通道（词表 API 会整请求拒绝）。
TENCENT_WORD_RE = re.compile(r"^[0-9A-Za-z一-鿿_-]+$")

# 生成句模板（轮换覆盖中英混合/纯中文场景，{word} 占位）。
SENTENCE_TEMPLATES = [
    "请把 {word} 加入文档。",
    "我们项目中用到了 {word}。",
    "把 {word} 写进 README。",
    "{word} 是什么工具？",
    "今天要处理{word}的问题。",
]


def sanitize_id(word: str) -> str:
    """词 -> 文件名安全 id（非中英文/数字/_- 的字符替换为 _）。"""
    return re.sub(r"[^0-9A-Za-z一-鿿_-]", "_", word) or "word"


def build_generate_manifest(hotwords: list[str], per_word: int) -> list[dict]:
    """为每个热词造句生成清单：每条 {id,text,hotwords}，供 gen_corpus 合成。"""
    items: list[dict] = []
    for word in hotwords:
        for n in range(per_word):
            template = SENTENCE_TEMPLATES[n % len(SENTENCE_TEMPLATES)]
            text = template.replace("{word}", word)
            items.append({
                "id": f"acc_{sanitize_id(word)}_{n + 1}",
                "text": text,
                "hotwords": [word],
            })
    return items


def load_clips(manifest: list[dict], base: Path) -> list[dict]:
    """加载音频清单：显式 audio 路径（用户 manifest）或 {id}.ogg（生成产物）。
    时长优先取同目录 .pcm（32kbps s16le mono 采样率 16000 → size/32000 秒），
    否则按 Ogg Opus 32kbps 从文件大小估算（仅用于发送节奏，不影响识别）。
    """
    items: list[dict] = []
    for item in manifest:
        ogg = Path(item.get("audio", base / f"{item['id']}.ogg"))
        if not ogg.exists():
            print(f"WARN 跳过 {item['id']}: 音频不存在 {ogg}", file=sys.stderr)
            continue
        pcm = ogg.with_suffix(".pcm")
        if pcm.exists():
            duration = pcm.stat().st_size / 32000.0
        else:
            duration = ogg.stat().st_size * 8.0 / 32000.0
        items.append({**item, "_ogg": ogg, "_duration": duration})
    return items


async def generate_clips(items: list[dict], out_dir: Path, voice: str,
                         force: bool) -> None:
    """edge-tts + ffmpeg 合成（复用 gen_corpus.gen_one），产物到 out_dir。"""
    ffmpeg = gen_corpus.find_ffmpeg(None)
    if not ffmpeg:
        raise RuntimeError("ffmpeg not found（需在 PATH 或安装 ffmpeg）")
    out_dir.mkdir(parents=True, exist_ok=True)
    ok = skip = 0
    with tempfile.TemporaryDirectory() as td:
        tmpdir = Path(td)
        for item in items:
            try:
                result = await gen_corpus.gen_one(item, voice, ffmpeg,
                                                  tmpdir, force,
                                                  corpus_dir=out_dir)
                if result == "skip":
                    skip += 1
                else:
                    ok += 1
            except Exception as e:  # noqa: BLE001 - 逐条收集失败不中断
                print(f"FAIL 合成 {item['id']}: {e}", file=sys.stderr)
    print(f"合成完成: ok={ok} skip={skip}（{out_dir}）")
    if ok + skip != len(items):
        raise RuntimeError(f"合成失败 {len(items) - ok - skip} 条")


def build_stats(hotwords: list[str],
                stats_file: Path | None) -> list[hotword_select.HotwordStat]:
    """构建评分统计：--stats-file 提供真实使用统计（hotword_usage.json），
    缺失的词按 manual（与桌面端 RankHotwords 库外词按 manual 一致）。"""
    loaded: dict[str, hotword_select.HotwordStat] = {}
    if stats_file and stats_file.exists():
        for s in hotword_select.load_stats_json(str(stats_file)):
            loaded[s.word] = s
    return [loaded.get(w, hotword_select.HotwordStat(word=w, source="manual"))
            for w in hotwords]


def desktop_extra(provider: str, hotwords: list[str],
                  stats: list[hotword_select.HotwordStat], cfg: dict[str, str]
                  ) -> tuple[dict | None, dict]:
    """桌面端同款发送参数。返回 (run_clip 关键字参数 或 None=无可发送, 诊断信息)。"""
    if provider == "tencent":
        sendable = [w for w in hotwords if TENCENT_WORD_RE.match(w)]
        unsendable = [w for w in hotwords if not TENCENT_WORD_RE.match(w)]
        if not sendable:
            return None, {"unsendable": unsendable, "sent": [],
                          "note": "无词可入腾讯热词表（含空格/'.' 的词只能靠 LLM 精修）"}
        vocab_id = tencent_vocab.sync_vocab(
            "VoiceStick-Acceptance", sendable,
            secret_id=cfg["tencent_secret_id"], secret_key=cfg["tencent_secret_key"],
            description="热词验收自动同步（可删除）")
        if not vocab_id:
            return None, {"unsendable": unsendable, "sent": sendable,
                          "note": "腾讯词表同步失败"}
        return {"hotword_id": vocab_id}, {"unsendable": unsendable,
                                          "sent": sendable, "vocab_id": vocab_id}
    # volcengine：评分排序 + 80 tokens 预算（与桌面端 RankedHotwordsForAsr 一致）
    ranked = hotword_select.rank(stats)
    direct = hotword_select.fit_token_budget(ranked)
    dropped = [w for w in ranked if w not in set(direct)]
    if not direct:
        return None, {"sent": [], "dropped": ranked, "note": "无词装入 80 tokens 预算"}
    return {"hotwords": direct}, {"sent": direct, "dropped": dropped}


def run_one(provider: str, item: dict, round_no: int, cfg: dict[str, str],
            timeout: float, extra: dict) -> ClipResult:
    common = dict(duration_s=item["_duration"], clip_id=item["id"], round_no=round_no,
                  category=item.get("category", ""), reference=item["text"])
    if provider == "volcengine":
        return volcengine.run_clip(item["_ogg"], api_key=cfg["volcengine_api_key"],
                                   timeout=timeout, **common, **extra)
    return tencent.run_clip(item["_ogg"], secret_id=cfg["tencent_secret_id"],
                            secret_key=cfg["tencent_secret_key"],
                            appid=cfg["tencent_appid"], timeout=timeout,
                            **common, **extra)


def miss_examples(results: list[ClipResult] | list[dict],
                  clip_hotwords: dict[str, list[str]]) -> dict[str, list[dict]]:
    """逐词未命中示例（参考文本 vs 识别原文），每词最多 3 条，供诊断下钻。
    接受 ClipResult 对象或 to_dict() 字典（报告重生成用）。"""
    examples: dict[str, list[dict]] = {}
    for r in results:
        if isinstance(r, dict):
            if not r.get("success"):
                continue
            final_text = r.get("final_text", "")
            reference = r.get("reference", "")
            clip_id = r.get("clip_id", "")
        else:
            if not r.success:
                continue
            final_text = r.final_text
            reference = r.reference
            clip_id = r.clip_id
        for word in clip_hotwords.get(clip_id, []):
            if hotword_hit(final_text, word):
                continue
            ex = {"reference": reference, "final_text": final_text,
                  "clip_id": clip_id}
            lst = examples.setdefault(word, [])
            if len(lst) < 3 and not any(e["clip_id"] == clip_id for e in lst):
                lst.append(ex)
    return examples


def build_diagnosis(report: dict, providers: list[str], hotwords: list[str],
                    clip_hotwords: dict[str, list[str]],
                    unsendable_all: list[str]) -> dict:
    """逐词基线 vs 桌面命中率 + 未命中原文示例（新跑与 --report-only 共用）。"""
    diagnosis: dict = {"miss_examples": {}, "per_word_delta": {}}
    for prov in providers:
        base = report["groups"].get("baseline", {}).get(prov)
        desk = report["groups"].get("desktop", {}).get(prov)
        if not desk:
            continue
        desk_per_word = desk["hotwords"]["per_word"]
        base_per_word = (base["hotwords"]["per_word"] if base else {})
        for word in hotwords:
            dw = desk_per_word.get(word, {})
            bw = base_per_word.get(word, {})
            dh = dw.get("hit", 0) / dw["total"] if dw.get("total") else None
            bh = bw.get("hit", 0) / bw["total"] if bw.get("total") else None
            delta = round(dh - bh, 4) if dh is not None and bh is not None else None
            diagnosis["per_word_delta"][f"{word}@{prov}"] = {
                "baseline": bh, "desktop": dh, "delta": delta,
                "unsendable": word in unsendable_all,
            }
        for word, examples in miss_examples(desk["detail"], clip_hotwords).items():
            diagnosis["miss_examples"][f"{word}@{prov}"] = examples
    return diagnosis


def write_report(report: dict, out_dir: Path, stamp: str) -> tuple[Path, Path]:
    out_dir.mkdir(parents=True, exist_ok=True)
    json_path = out_dir / f"acceptance_{stamp}.json"
    md_path = out_dir / f"acceptance_{stamp}.md"
    json_path.write_text(json.dumps(report, ensure_ascii=False, indent=2),
                         encoding="utf-8")
    lines = [
        f"# 热词验收报告 {stamp}",
        "",
        f"- 热词：{', '.join(report['hotwords'])}",
        f"- 音频：{report['clips']} 条（生成/标注），轮次 {report['rounds']}",
        f"- 不可入表（仅能靠 LLM 精修）：{', '.join(report['unsendable']) or '无'}",
        "",
        "## 命中率",
        "",
        "| 组 | 平台 | 成功率 | 热词命中率 | CER 均值 |",
        "|---|---|---|---|---|",
    ]
    for group, providers in report["groups"].items():
        for provider, block in providers.items():
            hw = block["hotwords"]["overall"]
            s = block["summary"]
            lines.append(
                f"| {group} | {provider} | {s['success']}/{s['runs']} "
                f"| {hw['hit']}/{hw['total']} ({hw['recall']}) "
                f"| {s['cer_mean']} |")
    lines += ["", "## 逐词命中率", ""]
    for group, providers in report["groups"].items():
        for provider, block in providers.items():
            lines.append(f"### {group} / {provider}")
            for word, s in block["hotwords"]["per_word"].items():
                lines.append(f"- {word}: {s['hit']}/{s['total']} ({s['recall']})")
            lines.append("")
    lines += ["", "## 诊断（未命中的识别原文）", ""]
    for word, examples in report["diagnosis"].get("miss_examples", {}).items():
        lines.append(f"### {word}")
        for ex in examples:
            lines.append(f"- 参考：{ex['reference']}")
            lines.append(f"  识别：{ex['final_text']}")
        if not examples:
            lines.append("- 无未命中")
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
    ap.add_argument("--provider", choices=["all", "volcengine", "tencent"],
                    default="", help="默认取 config.toml 的 asr_provider")
    ap.add_argument("--hotwords", default="", help="逗号分隔热词；默认读 config.toml")
    ap.add_argument("--manifest", default="", help="已有音频标注清单 JSON（有则跳过生成）")
    ap.add_argument("--sentences-per-word", type=int, default=3,
                    help="每个热词生成的测试句数（默认 3）")
    ap.add_argument("--rounds", type=int, default=2)
    ap.add_argument("--force", action="store_true", help="重新合成已有音频")
    ap.add_argument("--stats-file", default="", help="hotword_usage.json 路径（火山评分用）")
    ap.add_argument("--no-baseline", action="store_true", help="只跑 desktop 组")
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--out", default=str(DEFAULT_OUT))
    ap.add_argument("--stamp", default=datetime.now().strftime("%Y%m%d-%H%M%S"))
    ap.add_argument("--report-only", default="",
                    help="从部分结果 JSON 重生成报告（不调用 ASR，用于断点续报）")
    args = ap.parse_args()

    if args.report_only:
        path = Path(args.report_only)
        if not path.exists():
            print(f"部分结果不存在: {path}", file=sys.stderr)
            return 2
        report = json.loads(path.read_text(encoding="utf-8"))
        providers = [p for p in report.get("provider", "").split(",") if p]
        hotwords = report["hotwords"]
        # 不可入表诊断：一律重算（部分结果 JSON 里的 unsendable 是循环中临时值，不可信）
        unsendable = ([w for w in hotwords if "tencent" in providers
                       and not TENCENT_WORD_RE.match(w)]
                      if "tencent" in providers else [])
        report["unsendable"] = sorted(set(unsendable))
        # clip -> 目标热词映射：优先用新版本已存的 clip_hotwords，否则用 --manifest 补齐
        clip_hotwords = report.get("clip_hotwords", {})
        if not clip_hotwords and args.manifest:
            manifest_path = Path(args.manifest)
            if manifest_path.exists():
                manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
                clip_hotwords = {item["id"]: item.get("hotwords", [])
                                 for item in manifest}
        report["diagnosis"] = build_diagnosis(report, providers, hotwords,
                                              clip_hotwords, unsendable)
        json_path, md_path = write_report(report, Path(args.out), report["stamp"])
        print(f"JSON: {json_path}")
        print(f"MD:   {md_path}")
        return 0

    cfg = load_windows_config()
    provider = args.provider or cfg.get("asr_provider", "")
    providers = ["volcengine", "tencent"] if provider == "all" else [provider]
    if not providers or providers == [""]:
        print("未指定 provider 且 config.toml 无 asr_provider", file=sys.stderr)
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

    if args.hotwords.strip():
        hotwords = [w.strip() for w in args.hotwords.split(",") if w.strip()]
    else:
        raw = cfg.get("asr_hotwords", "")
        hotwords = [w.strip() for w in re.split(r"[,\n]", raw) if w.strip()]
    if not hotwords:
        print("热词为空（--hotwords 或 config.toml asr_hotwords）", file=sys.stderr)
        return 2

    # 音频来源：用户 manifest 优先，否则自动生成
    if args.manifest:
        manifest_path = Path(args.manifest)
        if not manifest_path.exists():
            print(f"manifest 不存在: {manifest_path}", file=sys.stderr)
            return 2
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        clips = load_clips(manifest, manifest_path.resolve().parent)
        audio_note = f"用户清单 {manifest_path.name}"
    else:
        stamp = args.stamp
        accept_dir = HERE / "corpus" / f"acceptance_{stamp}"
        manifest = build_generate_manifest(hotwords, args.sentences_per_word)
        try:
            asyncio.run(generate_clips(manifest, accept_dir, "zh-CN-XiaoxiaoNeural",
                                       args.force))
        except RuntimeError as e:
            print(f"FAIL: {e}", file=sys.stderr)
            return 2
        (accept_dir / "manifest.json").write_text(
            json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
        clips = load_clips(manifest, accept_dir)
        audio_note = f"生成音频 {accept_dir.name}"
    if not clips:
        print("无可用音频（清单为空或全部缺失）", file=sys.stderr)
        return 2
    clip_hotwords = {item["id"]: item.get("hotwords", []) for item in clips}

    stats = build_stats(hotwords, Path(args.stats_file) if args.stats_file else None)
    groups = ["desktop"] if args.no_baseline else ["baseline", "desktop"]

    report: dict = {
        "stamp": args.stamp,
        "provider": ",".join(providers),
        "hotwords": hotwords,
        "clips": len(clips),
        "audio_note": audio_note,
        "rounds": args.rounds,
        "unsendable": [],
        "sent": {},
        "clip_hotwords": clip_hotwords,
        "groups": {},
    }
    unsendable_all: list[str] = []

    for group in groups:
        for prov in providers:
            if group == "baseline":
                extra: dict | None = {}
                diag: dict = {"sent": hotwords}
            else:
                extra, diag = desktop_extra(prov, hotwords, stats, cfg)
                unsendable_all.extend(diag.get("unsendable", []))
                report["sent"][prov] = diag.get("sent", [])
                if diag.get("note"):
                    print(f"NOTE desktop/{prov}: {diag['note']}", file=sys.stderr)
            if extra is None:
                print(f"SKIP desktop/{prov}（无可发送热词）")
                continue
            results: list[ClipResult] = []
            total = args.rounds * len(clips)
            done = 0
            for round_no in range(1, args.rounds + 1):
                for item in clips:
                    done += 1
                    res = run_one(prov, item, round_no, cfg, args.timeout, extra)
                    results.append(res)
                    tag = "OK " if res.success else "FAIL"
                    note = (f" cer={cer(res.reference, res.final_text):.3f}"
                            if res.success else f" err={res.error}")
                    print(f"[{group}/{prov} {done}/{total}] {tag} "
                          f"r{round_no} {item['id']}{note}", flush=True)
            report["groups"].setdefault(group, {})[prov] = {
                "params": {k: (v if isinstance(v, (str, int, bool)) else f"<{len(v)} 词>")
                           for k, v in extra.items()},
                "summary": summarize(results),
                "hotwords": hotword_report(results, clip_hotwords, hotwords),
                "detail": [r.to_dict() for r in results],
            }
            hw = report["groups"][group][prov]["hotwords"]["overall"]
            print(f"== {group}/{prov}: 热词命中 {hw['hit']}/{hw['total']} "
                  f"({hw['recall']})", flush=True)
            partial = Path(args.out) / f"acceptance_{args.stamp}_partial.json"
            partial.parent.mkdir(parents=True, exist_ok=True)
            partial.write_text(json.dumps(report, ensure_ascii=False, indent=2),
                               encoding="utf-8")

    if not report["groups"]:
        print("无任何可运行组", file=sys.stderr)
        return 1

    report["unsendable"] = sorted(set(unsendable_all))
    report["diagnosis"] = build_diagnosis(report, providers, hotwords,
                                          clip_hotwords, unsendable_all)

    json_path, md_path = write_report(report, Path(args.out), args.stamp)
    print(f"\nJSON: {json_path}")
    print(f"MD:   {md_path}")
    print("\n逐词结论：")
    for key, d in sorted(report["diagnosis"]["per_word_delta"].items()):
        tag = "（不可入表，需 LLM 精修）" if d["unsendable"] else ""
        delta = f"{d['delta']:+.4f}" if d["delta"] is not None else "—"
        print(f"  {key}: baseline={d['baseline']} desktop={d['desktop']} "
              f"增益={delta}{tag}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
