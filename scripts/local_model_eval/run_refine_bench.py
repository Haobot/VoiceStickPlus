#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""本地 LLM 文本精修评测：ASR 输出 → 本地 ollama 精修 → CER 变化。

回答三个问题：
1. 本地小模型能否修复 SenseVoice 的 ASR 错字（含热词偏置下的术语修复）？
2. 本地小模型会不会把正确的文本改坏（损伤率=核心安全指标）？
3. 本地小模型能否完成口语清理（填充词/叠字/口头语）？

方法：
- 输入 = SenseVoice（Windows 在发同款 int8 模型）对本仓 31 条语料的第一轮输出；
  错误集即真实 ASR 错误（确定性漂移），不是手工编造。
- prompt 双轨均逐字取自生产代码：
  - CLOUD_PROMPT：Windows llm_refinement_client.cc BuildRefinePrompt 默认段
    （= macOS LLMRefinementClient 同款），指令式，云端大模型在用；
  - LOCAL_FEWSHOT：Windows local_refinement_client.cc BuildSystemPrompt，
    10 例 few-shot，为 0.6B/1.7B 设计；
  - HOTWORD_SUFFIX：BuildRefinePrompt 热词表后缀（逐字）。
- 请求体对齐生产：temperature=0、think:false（qwen3 家族关思考）、
  messages=[system, user(ASR原始文本)]；num_predict=512 仅为评测护栏。
- 填充词清理集：12 例合成输入（向标注文本注入嗯/那个/叠字等），独立打分。

指标：pipeline CER 均值/最差、相对 ASR 原始 CER 的改善/损伤计数、
clean-damaged（原文 0 错被改错）、tech 类 CER（热词效应）、解析失败数、延迟 p50/p95。

用法：
    .venv/bin/python run_refine_bench.py --models qwen2.5:1.5b-instruct qwen3:4b ...
结果写入 results/refine_<ts>.{json,md}
"""
from __future__ import annotations

import argparse
import json
import re
import statistics
import time
import urllib.request
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

import sys

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(REPO / "scripts" / "e2e_test"))
sys.path.insert(0, str(HERE))

from asr_bench.metrics import cer  # noqa: E402
import run_asr_bench as asr_bench  # noqa: E402

RESULTS_DIR = HERE / "results"
OLLAMA_URL = "http://127.0.0.1:11434"

# ---- 生产 prompt 逐字拷贝（改这里前先读文件头注释）----

CLOUD_PROMPT = (
    "你是一个语音识别后处理器。\n"
    "输入为自动语音识别生成的原始文本。请将其改写为规范的书面文本：\n"
    "\n"
    "• 去除因语音停顿产生的多余空格，尤其中文、日文、韩文字符间的空格；保留单词间及行内拉丁字母、数字周围的合法空格。\n"
    "\n"
    "• 修正标点：按文本语言规范补全缺失标点、调整错位标点、删除冗余标点。若输入仅为短语或短词，末尾请勿添加句号。\n"
    "\n"
    "• 剔除无实际语义的填充词、半截话、口吃及无意义口语碎片（如“嗯”“啊”“那个”“就是”“uh”“um”“you know”）。\n"
    "\n"
    "• 保留原意、语种与语气，不翻译或扩写内容。\n"
    "\n"
    "• 专有名词、数字、代码及专业术语保持不变。\n"
    "\n"
    "仅返回清理后的文本，无需解释、引号、前缀、备选方案或 Markdown 格式。"
)

HOTWORD_SUFFIX = (
    "\n\n用户常用术语热词表：{words}。若识别文本中出现与热词发音相近的写法"
    "（如拼读、同音、分写或连写变形），请替换为热词原形；没有相近发音的词时"
    "不要凭空插入热词。示例：热词表含「AGENTS.md」时，识别文本「编辑 "
    "agentsdmd 这个文件」应改为「编辑 AGENTS.md 这个文件」。原文中已经正确"
    "出现的热词必须原样保留，不得改写。"
)

LOCAL_FEWSHOT = (
    "清理语音识别文本：删除无意义的填充词、连接口头语和口吃重复"
    "（嗯/啊/呃/哦/那个/这个/就是/然后/呢/吧/uh/um/you know/叠字），"
    "修正标点与多余空格，保留原意、语种、专有名词与数字，不增删实义内容。"
    "只输出清理后的文本。\n"
    "\n"
    "输入：嗯，帮我把这个文件重命名一下。\n"
    "输出：帮我把这个文件重命名一下。\n"
    "\n"
    "输入：啊，那个，我们还是用那个 SenseVoice 吧。\n"
    "输出：我们还是用 SenseVoice 吧。\n"
    "\n"
    "输入：嗯，明天下午三点提醒我开会。\n"
    "输出：明天下午三点提醒我开会。\n"
    "\n"
    "输入：然后呢，我们下午再去那个超市吧。\n"
    "输出：我们下午再去超市吧。\n"
    "\n"
    "输入：你把那个设置页面里的 debug 那个开关打开就行了。\n"
    "输出：你把设置页面里的 debug 开关打开就行了。\n"
    "\n"
    "输入：行行行，我明白了。\n"
    "输出：行，我明白了。\n"
    "\n"
    "输入：我我我想去吃火锅。\n"
    "输出：我想去吃火锅。\n"
    "\n"
    "输入：嗯 呃 这个项目 嗯 用的是 BLE 连接\n"
    "输出：这个项目用的是 BLE 连接。\n"
    "\n"
    "输入：I think um we should uh use the model.\n"
    "输出：I think we should use the model.\n"
    "\n"
    "输入：帮我在 GitHub 上搜一下 llama.cpp 这个项目。\n"
    "输出：帮我在 GitHub 上搜一下 llama.cpp 这个项目。"
)

# 语料 tech/mix 项真实术语构成的「用户热词表」——模拟真实用户配置
HOTWORDS = ["ASR", "Opus", "Ogg", "GitHub", "pull request", "BLE",
            "ESP32", "WebSocket", "API", "SDK", "CPU", "python"]

# 受限编辑指令形态（第 4 臂）：模型只出「错词→热词 / 删除：片段」指令，
# 代码层守卫执行（纠正词必须在热词表、错词/片段必须真实存在于原文）。
# 形态移植自 Windows BuildCorrectionSystemPrompt（跨轮纠错 GO 口径）+
# targeted 对齐的「候选约束式」house pattern。
EDITFORM_PROMPT = (
    "你是语音识别结果纠错器。输入识别文本与用户热词表，只输出修改指令，"
    "每行一条，别的不写：\n"
    "1) 替换：错词→热词原形。错词是文本中实际出现的错误写法"
    "（同音/近音/大小写/分写/连写变形），箭头右边必须是热词表里的词。\n"
    "2) 删除：原样摘录要删除的无意义片段（填充词、口吃重复，如 嗯、啊、"
    "那个、我我）。\n"
    "没有要改的只输出：无。禁止重写整句，禁止输出指令之外的任何内容。\n"
    "\n"
    "热词表：AGENTS.md、GitHub、蓝牙\n"
    "输入：嗯，帮我编辑 agentsdmd 这个文件，然后用蓝崖遥控器测试。\n"
    "处理：\n"
    "删除：嗯，\n"
    "agentsdmd→AGENTS.md\n"
    "蓝崖→蓝牙\n"
    "\n"
    "热词表：SDK、IDE\n"
    "输入：把skd装进die里面。\n"
    "处理：\n"
    "skd→SDK\n"
    "die→IDE\n"
    "\n"
    "热词表：Ogg、ASR\n"
    "输入：把ASR的音频封装成OG格式再发送。\n"
    "处理：\n"
    "OG→Ogg"
)

VARIANTS = {
    # name: system prompt 构造器
    "cloud": lambda: CLOUD_PROMPT,
    "cloud+hw": lambda: CLOUD_PROMPT + HOTWORD_SUFFIX.format(words=", ".join(HOTWORDS)),
    "fewshot": lambda: LOCAL_FEWSHOT,
    "fewshot+hw": lambda: LOCAL_FEWSHOT + HOTWORD_SUFFIX.format(words=", ".join(HOTWORDS)),
    "editform": lambda: EDITFORM_PROMPT + "\n\n热词表：" + "、".join(HOTWORDS),
}

_MAX_EDITS = 5


def apply_edit_instructions(text: str, raw_output: str,
                            hotwords: list[str] | None = None) -> str:
    """守卫执行 editform 指令：替换目标必须命中热词表，错词/片段必须真实存在。"""
    hotwords = hotwords or HOTWORDS
    hw_norm = {h.casefold(): h for h in hotwords}
    out = text
    applied = 0
    for line in raw_output.splitlines():
        line = line.strip().strip("`")
        if not line or line in ("无", "处理：", "处理:") or line.startswith(("输入", "热词表")):
            continue
        if applied >= _MAX_EDITS:
            break
        if line.startswith(("删除", "删除：", "删除:")):
            frag = line.split("：", 1)[-1].split(":", 1)[-1].strip()
            if 0 < len(frag) <= 10 and frag in out:
                out = out.replace(frag, "", 1)
                applied += 1
            continue
        if "→" in line:
            left, _, right = line.partition("→")
            left, right = left.strip().strip("`"), right.strip().strip("`").strip("。")
            target = hw_norm.get(right.casefold())
            if target and left and left != target:
                # 大小写不敏感定位（OG→Ogg 应命中原文中的 Og）
                low, pos = out.casefold(), out.casefold().find(left.casefold())
                if pos >= 0:
                    out = out[:pos] + target + out[pos + len(left):]
                    applied += 1
    return out


_CJK_RUN_RE = None  # lazy
_LEADING_FILLER_RE = None
_CJK_SPACE_RE = None


def deterministic_clean(text: str) -> str:
    """纯确定性清理（零 LLM）：句首填充词、3+ 连字口吃、CJK 间空格。

    句首剥离规则（r5 修正，tech_01 教训：「这个API」的「这个」是合法指示代词）：
    - 语气词 嗯/啊/呃/哦：后跟标点或空白即剥（真语气词几乎必有停顿）；
    - 指示/连接词 那个/这个/就是/然后呢：仅在后跟逗号时剥（无逗号视作实义）。
    """
    import re as _re
    global _CJK_RUN_RE, _LEADING_FILLER_RE, _CJK_SPACE_RE
    if _LEADING_FILLER_RE is None:
        interj = r"(?:嗯|啊|呃|哦)[，,、]?\s*"
        dem = r"(?:(?:那个|这个|就是|然后呢)[，,])"
        _LEADING_FILLER_RE = _re.compile(
            r"^(?:" + interj + r"|" + dem + r")+\s*")
        _CJK_RUN_RE = _re.compile(r"([\u4e00-\u9fff])\1{2,}")
        _CJK_SPACE_RE = _re.compile(r"(?<=[\u4e00-\u9fff])[ \t]+(?=[\u4e00-\u9fff])")
    for _ in range(3):
        new = _LEADING_FILLER_RE.sub("", text)
        if new == text:
            break
        text = new
    text = _CJK_RUN_RE.sub(r"\1", text)
    text = _CJK_SPACE_RE.sub("", text)
    return text

# 合成填充词清理集：expected 为清理后 ground truth
FILLER_CASES = [
    ("f01", "嗯，帮我打开浏览器。", "帮我打开浏览器。"),
    ("f02", "那个，今天天气不错。", "今天天气不错。"),
    ("f03", "我我我想去吃火锅。", "我想去吃火锅。"),
    ("f04", "嗯 呃 这个项目 嗯 用的是蓝牙连接", "这个项目用的是蓝牙连接。"),
    ("f05", "然后呢，明天下午三点提醒我开会。", "明天下午三点提醒我开会。"),
    ("f06", "啊，那个，现在的会议记录整理一下。", "现在的会议记录整理一下。"),
    ("f07", "行行行，我明白了。", "行，我明白了。"),
    ("f08", "I think um we should uh use the model.", "I think we should use the model."),
    ("f09", "嗯，请帮我把这个、那个文件打印三份。", "请帮我把这个文件打印三份。"),
    ("f10", "就是，下周三之前要完成代码审查。", "下周三之前要完成代码审查。"),
    ("f11", "嗯那个把cpu使用率报警阈值设为百分之八十", "把cpu使用率报警阈值设为百分之八十。"),
    ("f12", "呃，ESP32 通过 BLE 把音频数据发送到桌面端。", "ESP32 通过 BLE 把音频数据发送到桌面端。"),
]

THINK_RE = re.compile(r"<think>[\s\S]*?</think>|</?think>")


def clean_output(text: str) -> str:
    text = THINK_RE.sub("", text).strip()
    # 剥包引号（生产两端都做类似清理）
    for l, r in (("「", "」"), ("“", "”"), ('"', '"'), ("'''", "'''")):
        if text.startswith(l) and text.endswith(r) and len(text) > 2:
            text = text[1:-1].strip()
    return text


def ollama_chat(model: str, system: str, user: str) -> tuple[str, float]:
    body = json.dumps({
        "model": model,
        "messages": [
            {"role": "system", "content": system},
            {"role": "user", "content": user},
        ],
        "stream": False,
        "think": False,
        "options": {"temperature": 0, "num_predict": 512},
    }).encode()
    req = urllib.request.Request(
        OLLAMA_URL + "/api/chat", data=body,
        headers={"Content-Type": "application/json"})
    t0 = time.perf_counter()
    with urllib.request.urlopen(req, timeout=120) as resp:
        data = json.loads(resp.read())
    dt = (time.perf_counter() - t0) * 1000
    return clean_output(data.get("message", {}).get("content", "")), dt


@dataclass
class CaseResult:
    item_id: str
    variant: str
    raw_asr: str
    refined: str
    reference: str
    latency_ms: float
    parse_fail: bool = False


def run_variant(model: str, variant: str, inputs: list[dict]) -> list[CaseResult]:
    if variant == "det":
        # 纯确定性臂：无 LLM 调用
        return [CaseResult(it["id"], variant, it["asr_text"],
                           deterministic_clean(it["asr_text"]),
                           it["reference"], 0.0) for it in inputs]
    if variant == "det+editform":
        # 组合栈（生产推荐形态）：确定性清理 → 受限编辑指令
        system = VARIANTS["editform"]()
        out = []
        for it in inputs:
            base = deterministic_clean(it["asr_text"])
            try:
                raw, dt = ollama_chat(model, system, base)
                pf = not raw
            except Exception:  # noqa: BLE001
                raw, dt, pf = "", 0.0, True
            refined = base if pf else apply_edit_instructions(base, raw)
            out.append(CaseResult(it["id"], variant, it["asr_text"], refined,
                                  it["reference"], dt, pf))
        return out
    system = VARIANTS[variant]()
    out = []
    for it in inputs:
        try:
            raw, dt = ollama_chat(model, system, it["asr_text"])
            pf = not raw
        except Exception as e:  # noqa: BLE001
            raw, dt, pf = "", 0.0, True
            it.setdefault("_err", str(e))
        if pf:
            refined = it["asr_text"]  # 生产语义：失败回退原文
        elif variant == "editform":
            refined = apply_edit_instructions(it["asr_text"], raw)
        else:
            refined = raw
        out.append(CaseResult(it["id"], variant, it["asr_text"], refined,
                               it["reference"], dt, pf))
    return out


def summarize(model: str, variant: str, results: list[CaseResult],
              baseline_cers: dict[str, float]) -> dict:
    cers = {r.item_id: cer(r.reference, r.refined) for r in results}
    vals = list(cers.values())
    base_vals = [baseline_cers[r.item_id] for r in results]
    improved = sum(1 for r in results
                   if cers[r.item_id] < baseline_cers[r.item_id] - 0.01)
    damaged = sum(1 for r in results
                  if cers[r.item_id] > baseline_cers[r.item_id] + 0.01)
    clean_damaged = sum(1 for r in results
                        if baseline_cers[r.item_id] == 0 and cers[r.item_id] > 0.01)
    tech = [c for r in results if r.item_id.startswith("tech")
            for c in [cers[r.item_id]]]
    lat = sorted(r.latency_ms for r in results if r.latency_ms > 0)
    p50 = lat[len(lat) // 2] if lat else None
    p95 = lat[min(len(lat) - 1, round(0.95 * (len(lat) - 1)))] if lat else None

    def m(x):
        return round(statistics.mean(x), 4) if x else None

    return {
        "model": model, "variant": variant, "n": len(results),
        "cer_mean": m(vals), "cer_worst": round(max(vals), 4) if vals else None,
        "baseline_cer_mean": m(base_vals),
        "delta_mean": round(m(vals) - m(base_vals), 4) if vals else None,
        "improved": improved, "damaged": damaged, "clean_damaged": clean_damaged,
        "tech_cer_mean": m(tech),
        "parse_fail": sum(1 for r in results if r.parse_fail),
        "latency_p50_ms": round(p50) if p50 else None,
        "latency_p95_ms": round(p95) if p95 else None,
        "worst_cases": [
            {"item": r.item_id, "cer": round(cers[r.item_id], 4),
             "ref": r.reference[:24], "raw": r.raw_asr[:24], "out": r.refined[:24]}
            for r in sorted(results, key=lambda r: cers[r.item_id], reverse=True)[:5]],
    }


def make_asr_inputs() -> tuple[list[dict], dict[str, float]]:
    """跑 SenseVoice 第一轮拿确定性输出作为精修输入。"""
    corpus = asr_bench.load_corpus()
    res = asr_bench.run_sensevoice(corpus, rounds=1)
    inputs = []
    baseline = {}
    for it, ir in zip(corpus, res.items):
        inputs.append({"id": it["id"], "asr_text": ir.text,
                       "reference": it["reference"]})
        baseline[it["id"]] = cer(it["reference"], ir.text)
    return inputs, baseline


def run_filler_set(model: str, prompt_variant: str) -> dict:
    rows = []
    for cid, inp, exp in FILLER_CASES:
        pf = False
        if prompt_variant == "det":
            out, dt = deterministic_clean(inp), 0.0
        elif prompt_variant == "det+editform":
            base = deterministic_clean(inp)
            system = VARIANTS["editform"]()
            try:
                raw, dt = ollama_chat(model, system, base)
                pf = not raw
            except Exception:  # noqa: BLE001
                raw, dt, pf = "", 0.0, True
            out = base if pf else apply_edit_instructions(base, raw)
        else:
            system = VARIANTS[prompt_variant]()
            try:
                raw, dt = ollama_chat(model, system, inp)
                pf = not raw
            except Exception:  # noqa: BLE001
                raw, dt, pf = "", 0.0, True
            if pf:
                out = inp
            elif prompt_variant == "editform":
                out = apply_edit_instructions(inp, raw)
            else:
                out = raw
        rows.append((cid, cer(exp, out), pf))
    cers = [c for _, c, _ in rows]
    return {
        "model": model, "variant": prompt_variant,
        "cer_mean": round(statistics.mean(cers), 4),
        "perfect": sum(1 for c in cers if c == 0),
        "parse_fail": sum(1 for _, _, pf in rows if pf),
    }


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--models", nargs="+", required=True)
    ap.add_argument("--variants", nargs="+",
                    default=["cloud", "cloud+hw", "fewshot"])
    ap.add_argument("--filler-variant", default="fewshot")
    ap.add_argument("--skip-filler", action="store_true")
    args = ap.parse_args()

    print("generating SenseVoice inputs ...", flush=True)
    inputs, baseline = make_asr_inputs()
    print(f"inputs: {len(inputs)}, baseline cer_mean="
          f"{statistics.mean(baseline.values()):.4f}", flush=True)

    summaries, fillers = [], []
    for model in args.models:
        if model == "det":
            for variant in [v for v in args.variants if v == "det"]:
                results = run_variant(model, variant, inputs)
                s = summarize(model, variant, results,
                              {r.item_id: baseline[r.item_id] for r in results})
                summaries.append(s)
                print(f"[{model}/{variant}] cer={s['cer_mean']} "
                      f"base={s['baseline_cer_mean']} damage={s['damaged']}", flush=True)
            if not args.skip_filler:
                fillers.append(run_filler_set(model, args.filler_variant))
                print(f"[det/filler:{args.filler_variant}] {fillers[-1]}", flush=True)
            continue
        # 预热：加载模型（ollama 冷启动）
        try:
            ollama_chat(model, "预热。", "ok")
        except Exception as e:  # noqa: BLE001
            print(f"[{model}] WARMUP FAILED: {e}", flush=True)
            continue
        for variant in args.variants:
            results = run_variant(model, variant, inputs)
            s = summarize(model, variant, results,
                          {r.item_id: baseline[r.item_id] for r in results})
            summaries.append(s)
            print(f"[{model}/{variant}] cer={s['cer_mean']} base={s['baseline_cer_mean']} "
                  f"improve={s['improved']} damage={s['damaged']} "
                  f"clean_dmg={s['clean_damaged']} tech={s['tech_cer_mean']} "
                  f"p50={s['latency_p50_ms']}ms", flush=True)
        if not args.skip_filler:
            fillers.append(run_filler_set(model, args.filler_variant))
            print(f"[{model}/filler:{args.filler_variant}] "
                  f"{fillers[-1]}", flush=True)

    RESULTS_DIR.mkdir(exist_ok=True)
    ts = datetime.now().strftime("%Y%m%d-%H%M%S")
    out_json = RESULTS_DIR / f"refine_{ts}.json"
    out_md = RESULTS_DIR / f"refine_{ts}.md"
    out_json.write_text(json.dumps({
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "baseline_cer": {k: round(v, 4) for k, v in baseline.items()},
        "hotwords": HOTWORDS,
        "engines": summaries, "filler": fillers,
    }, ensure_ascii=False, indent=1), encoding="utf-8")

    lines = ["# 本地 LLM 精修评测报告", "",
             f"- 生成时间：{datetime.now().strftime('%Y-%m-%d %H:%M')}",
             "- 输入：SenseVoice int8 对 31 条语料的第一轮输出（确定性）",
             f"- 基线（ASR 原始）CER 均值：{statistics.mean(baseline.values()):.4f}",
             "- 指标：damage=改错数（ΔCER>+0.01）；clean_dmg=原文全对被改错；"
             "tech=术语类 CER（热词效应）", "",
             "## 主表（31 条 ASR 输入）", "",
             "| 模型 | 变体 | CER均值 | Δ基线 | 改善 | 损伤 | clean损伤 | tech类 | 解析失败 | 延迟p50/p95(ms) |",
             "|---|---|---|---|---|---|---|---|---|---|"]
    for s in summaries:
        lines.append(
            f"| {s['model']} | {s['variant']} | {s['cer_mean']:.4f} | "
            f"{s['delta_mean']:+.4f} | {s['improved']} | {s['damaged']} | "
            f"{s['clean_damaged']} | {s['tech_cer_mean']} | {s['parse_fail']} | "
            f"{s['latency_p50_ms']}/{s['latency_p95_ms']} |")
    if fillers:
        lines += ["", "## 填充词清理集（12 例合成输入）", "",
                  "| 模型 | prompt | CER均值 | 全对数/12 | 解析失败 |",
                  "|---|---|---|---|---|"]
        for f in fillers:
            lines.append(f"| {f['model']} | {f['variant']} | {f['cer_mean']:.4f} "
                         f"| {f['perfect']} | {f['parse_fail']} |")
    for s in summaries:
        lines += ["", f"## {s['model']}/{s['variant']} 最差 5 例", "",
                  "| 语料 | CER | 参考 | ASR原始 | 精修输出 |", "|---|---|---|---|---|"]
        for w in s["worst_cases"]:
            lines.append(f"| {w['item']} | {w['cer']:.4f} | {w['ref']} "
                         f"| {w['raw']} | {w['out']} |")
    out_md.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"written: {out_json}\n          {out_md}")


if __name__ == "__main__":
    main()
