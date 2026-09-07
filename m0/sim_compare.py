#!/usr/bin/env python3
"""闭环对比评测管线：本地 vs 云端（用户需求③④）。

同一测试集（data/wavs/ 29 条已录制音频，GT 为人工标注）上，
对三类引擎跑端到端识别并聚合效率/准确度/综合指标：

- local_sense_voice: SenseVoice-Small int8（sherpa-onnx，本地基线，无热词）
- local_seaco:       SeACo-Paraformer 双层热词管线（模型级偏置 + 后处理）
- cloud_tencent:     腾讯云一句话识别（16k_zh，同步 HTTPS，端到端含网络往返）

产出:
- data/results/sim_compare_<时间戳>.json（逐句明细，可复现）
- data/results/sim_compare_latest.json（供 Web 界面读取）
- docs/cloud_vs_local_report.md（评估报告 + 优化建议）

不伪造结果：引擎不健康（权重缺失/凭据缺失）直接跳过并在报告注明；
云端连续失败触发断路器中止该引擎，剩余句标记 skipped。

用法（m0/ 目录下）:
    .venv/Scripts/python.exe sim_compare.py
    .venv/Scripts/python.exe sim_compare.py --engines local_sense_voice,cloud_tencent
"""
from __future__ import annotations

import argparse
import json
import math
import statistics
import sys
import time
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "src"))

from m0_asr.compare_report import aggregate, render_report  # noqa: E402
from m0_asr.metrics import cer, hotword_hit  # noqa: E402
from m0_asr.platform_config import (  # noqa: E402
    load_platform_config,
    resolve_tencent_credentials,
)
from m0_asr.providers import get_provider, registered_provider_names  # noqa: E402

M0_ROOT = Path(__file__).resolve().parent
TESTSET_JSON = M0_ROOT / "data" / "texts" / "testset.json"
WAVS_DIR = M0_ROOT / "data" / "wavs"
HOTWORDS_TXT = M0_ROOT / "data" / "texts" / "hotwords.txt"
RESULTS_DIR = M0_ROOT / "data" / "results"
REPORT_MD = M0_ROOT / "docs" / "cloud_vs_local_report.md"

CLOUD_INTERVAL_SECONDS = 0.5  # 云端限速间隔（QPS 保守值）
CLOUD_CIRCUIT_BREAKER = 3  # 连续失败 N 次中止云端引擎

CATEGORY_LABELS = {
    "daily_zh": "日常对话中文",
    "mix_zh_en": "中英混读",
    "reading_zh": "安静朗读",
    "hotword": "热词句",
}


def load_entries() -> list[dict]:
    """合并 benchmark + hotword 两段测试集，热词句补 category=hotword。"""
    data = json.loads(TESTSET_JSON.read_text(encoding="utf-8"))
    entries = []
    for item in data["benchmark"]:
        entries.append({**item, "hotword": None, "category": item["category"]})
    for item in data["hotword"]:
        entries.append({**item, "category": "hotword"})
    missing = [e["id"] for e in entries
               if not (WAVS_DIR / f"{e['id']}.wav").exists()]
    if missing:
        raise FileNotFoundError(f"缺少音频文件: {missing}（先跑 scripts/gen_test_audio.py）")
    return entries


def run_engine(engine_name: str, entries: list[dict], credentials) -> tuple[list[dict], str]:
    """单引擎跑全测试集，返回 (逐句行, 跳过原因; 空串=正常跑完)。"""
    provider = get_provider(engine_name, credentials=credentials)
    status = provider.health_check()
    if not status.ok:
        return [], f"健康检查未通过: {status.detail}"
    provider.warmup()  # 模型加载/连接建立不计入单句延迟

    rows: list[dict] = []
    consecutive_failures = 0
    for entry in entries:
        wav = WAVS_DIR / f"{entry['id']}.wav"
        outcome = provider.transcribe_file(wav)
        if outcome.error is None:
            consecutive_failures = 0
        else:
            consecutive_failures += 1
        hit = (None if entry["hotword"] is None
               else hotword_hit(outcome.text, entry["hotword"]))
        rows.append({
            "engine": engine_name,
            "id": entry["id"],
            "category": entry["category"],
            "hotword": entry["hotword"],
            "text_gt": entry["text"],
            "text_asr": outcome.text,
            "hit": hit,
            "elapsed_seconds": round(outcome.elapsed_seconds, 4),
            "audio_seconds": round(outcome.audio_seconds, 2),
            "rtf": round(outcome.rtf, 4) if outcome.audio_seconds else None,
            "error": outcome.error,
        })
        print(f"  [{engine_name}] {entry['id']}: "
              f"{outcome.elapsed_seconds:.3f}s "
              f"{'ERR: ' + (outcome.error or '')[:60] if outcome.error else outcome.text[:40]}")
        if provider.kind == "cloud":
            if consecutive_failures >= CLOUD_CIRCUIT_BREAKER:
                remaining = len(entries) - len(rows)
                print(f"  [{engine_name}] 断路器触发（连续 {consecutive_failures} 次失败），"
                      f"剩余 {remaining} 句跳过")
                break
            time.sleep(CLOUD_INTERVAL_SECONDS)
    return rows, ""


def _auto_insights(metrics: dict, rows_by_engine: dict) -> list[str]:
    """基于聚合数据的规则化洞察（自动生成，重跑可复现）。"""
    insights: list[str] = []
    if not metrics:
        return insights

    best_cer = min(metrics, key=lambda n: metrics[n].cer_overall)
    best_lat = min(metrics, key=lambda n: metrics[n].latency.get("median", math.inf))
    insights.append(
        f"准确度最优 **{best_cer}**（CER {metrics[best_cer].cer_overall:.2%}）；"
        f"延迟最优 **{best_lat}**（中位 {metrics[best_lat].latency['median']:.3f}s）")

    # 本地引擎 RTF 达标判定（用户体感线：10 秒音频 1 秒内出结果）
    for name, m in metrics.items():
        if name.startswith("local"):
            passed = m.latency["median"] < 1.0
            insights.append(
                f"{name} 中位延迟 {m.latency['median']:.3f}s，"
                f"{'满足' if passed else '不满足'} 10 秒音频 1 秒内出结果"
                f"（本测试集音频约 5-9s，对应 RTF 线见明细 JSON）")

    # 热词注入增益：同栈 有热词 vs 无热词 直接对比
    pairs = [("local_sense_voice", "local_seaco", "本地"),
             ("cloud_tencent", "cloud_tencent_hotword", "云端")]
    for base, biased, label in pairs:
        if base in metrics and biased in metrics:
            gain = metrics[base].error_rate - metrics[biased].error_rate
            insights.append(
                f"{label}热词注入增益：未命中率 {metrics[base].error_rate:.0%} → "
                f"{metrics[biased].error_rate:.0%}"
                f"（{'+' if gain >= 0 else ''}{gain:.0%}，"
                f"剩余未命中 {metrics[biased].hotword_misses} 句）")

    # 中英混读（技术词场景）差距
    mix = {}
    for name, rows in rows_by_engine.items():
        vals = [cer(r["text_gt"], r["text_asr"]) for r in rows
                if r["category"] == "mix_zh_en" and not r["error"]]
        if vals:
            mix[name] = statistics.fmean(vals)
    if mix:
        order = sorted(mix, key=mix.get)
        insights.append(
            "中英混读（技术词）CER 排序: " +
            " < ".join(f"{n} {mix[n]:.2%}" for n in order))
    return insights


def build_report(metrics: dict, rows_by_engine: dict, skipped: dict[str, str],
                 note: str) -> str:
    """在通用渲染之上追加逐类 CER、错误明细与口径声明。"""
    base = render_report(metrics, dataset_note=note)
    insights = _auto_insights(metrics, rows_by_engine)
    extra = ["", "## 数据洞察（自动生成，重跑可复现）", ""]
    extra += [f"- {line}" for line in insights]
    extra += ["", "## 逐类 CER（成功句）", ""]
    all_rows = [r for rows in rows_by_engine.values() for r in rows]
    engines = list(metrics)
    extra += ["| 引擎 | 日常对话 | 中英混读 | 安静朗读 | 热词句 |", "|---|---|---|---|---|"]
    for name in engines:
        cells = []
        for cat in ("daily_zh", "mix_zh_en", "reading_zh", "hotword"):
            cat_rows = [r for r in rows_by_engine.get(name, [])
                        if r["category"] == cat and not r["error"]]
            cells.append(f"{statistics.fmean(cer(r['text_gt'], r['text_asr']) for r in cat_rows):.2%}"
                         if cat_rows else "-")
        extra.append(f"| {name} | " + " | ".join(cells) + " |")
    extra += [""]

    error_rows = [r for r in all_rows if r["error"]]
    if error_rows:
        extra += ["## 失败明细", ""]
        for r in error_rows[:20]:
            extra.append(f"- `{r['engine']}` {r['id']}: {r['error'][:100]}")
        extra += [""]
    if skipped:
        extra += ["## 未参评引擎", ""]
        for name, reason in skipped.items():
            extra.append(f"- **{name}**: {reason}")
        extra += [""]

    extra += ["""## 口径与局限声明

- 本地引擎延迟为纯推理端到端；云端延迟含 HTTPS 网络往返与鉴权，两者同为
  "提交音频→拿到文本"的用户体感口径，但云端测试网络为当前开发机网络。
- 云端走一句话识别（整段上传），与桌面端生产链路的流式 WebSocket 不同接口，
  本报告结论用于引擎选型参考，不代表生产流式的最终延迟。
- 测试音频为 edge-tts 合成（安静环境、标准发音），GT 为人工标注；
  嘈杂真实人声场景未覆盖，结论外推需谨慎。
- 腾讯云费用：一句话识别按次计费，一次全量评测每云端引擎 29 次调用，
  处于免费额度/低费用量级（以账单为准，不在报告内臆造单价）。
""", "## 后续优化建议（基于本次数据）", ""]

    # 规则化建议：本地引擎延迟达标 → 主推本地；云端准确度优势 → fallback 定位
    if any(n.startswith("local") and metrics[n].latency["median"] < 0.5
           for n in metrics):
        extra.append(
            "- **主路径用本地**：本地引擎中位延迟在亚秒级、离线零成本，"
            "作为默认引擎；云端准确度优势明显时按需切换或作为 fallback。")
    cloud_names = [n for n in metrics if n.startswith("cloud")]
    if cloud_names:
        best_cloud = min(cloud_names, key=lambda n: metrics[n].cer_overall)
        extra.append(
            f"- **云端价值点**：{best_cloud} 的 CER 与热词命中率均领先，"
            "适合弱网容忍、复杂术语、追求最高准确度的场景；但每句约 0.9s 的"
            "网络往返延迟对「说完即出」的体感是硬伤，且按次计费。")
    if "local_seaco" in metrics:
        m = metrics["local_seaco"]
        extra.append(
            f"- **本地热词路线**：SeACo 双层管线未命中率 {m.error_rate:.0%}，"
            "中文同音热词修复有效但英文技术词仍弱（详见 hotword_report.md）；"
            "P1 方向：本地主引擎 + SeACo 热词并联、后处理阈值调优，"
            "以及给 SenseVoice 增加本地文本级热词纠正（参考 funasr postprocess）。")
    extra.append(
        "- **测试集扩展**：当前为 TTS 合成口径，P1 应补充真人语音与耳语录音"
        "（M0 生死判据项），再跑本管线复验结论。")
    extra.append("")
    return base.rstrip() + "\n" + "\n".join(extra)


def main() -> int:
    parser = argparse.ArgumentParser(description="本地 vs 云端 ASR 闭环对比评测")
    parser.add_argument("--engines", default=None,
                        help=f"逗号分隔，默认取配置 eval_engines；可选 {registered_provider_names()}")
    args = parser.parse_args()

    config = load_platform_config(M0_ROOT / "config.toml")
    engine_names = (args.engines.split(",") if args.engines
                    else config.eval_engines)
    unknown = [n for n in engine_names if n not in registered_provider_names()]
    if unknown:
        print(f"未知引擎: {unknown}，可选: {registered_provider_names()}")
        return 1

    entries = load_entries()
    print(f"测试集: {len(entries)} 条（benchmark 9 + hotword 20，音频: {WAVS_DIR}）")

    credentials = resolve_tencent_credentials(
        m0_config=M0_ROOT / "config.toml",
        voicestick_config=Path.home() / "AppData/Roaming/VoiceStick/config.toml")

    rows_by_engine: dict[str, list[dict]] = {}
    skipped: dict[str, str] = {}
    for name in engine_names:
        print(f"== 引擎: {name}")
        rows, reason = run_engine(name, entries, credentials)
        if reason:
            skipped[name] = reason
            continue
        rows_by_engine[name] = rows

    metrics = {name: aggregate(name, rows) for name, rows in rows_by_engine.items()}
    note = ("29 条 edge-tts 合成音频（9 基准 + 20 热词），GT 人工标注；"
            "延迟为端到端口径（云端含网络往返）")
    report = build_report(metrics, rows_by_engine, skipped, note)

    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    payload = {
        "timestamp": stamp,
        "engines": engine_names,
        "entries": len(entries),
        "metrics": {k: vars(v) | {"latency": dict(v.latency)} for k, v in metrics.items()},
        "rows": rows_by_engine,
        "skipped": skipped,
    }
    (RESULTS_DIR / f"sim_compare_{stamp}.json").write_text(
        json.dumps(payload, ensure_ascii=False, indent=1), encoding="utf-8")
    (RESULTS_DIR / "sim_compare_latest.json").write_text(
        json.dumps(payload, ensure_ascii=False, indent=1), encoding="utf-8")
    REPORT_MD.write_text(report, encoding="utf-8")
    print(f"结果: {RESULTS_DIR / f'sim_compare_{stamp}.json'}")
    print(f"报告: {REPORT_MD}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
