#!/usr/bin/env python3
"""精修链路延迟基准探测（refine-latency-optimization 方案 A2）。

请求体与桌面端 LLMRefinementClient.makeRequest 完全一致（默认精修 prompt +
stream:true + temperature:0 + max_tokens + 关思考字段），4 档真实长度中文口语
文本 × N 次，输出 TTFT/总耗时 p50/p95、tok/s（delta 块速近似）、热词保序与
内容丢失启发式，并打印全文供人工判读。

对照组（DeepSeek 现配置）key 从本机 config.toml 只读、只进请求头、绝不打印。

用法:
    python3 scripts/probe_refine_latency.py                       # 读 config.toml 的现配置
    python3 scripts/probe_refine_latency.py --base-url http://127.0.0.1:11434/v1 \
        --model qwen2.5:3b-instruct --api-key ollama --runs 8
"""

from __future__ import annotations

import argparse
import json
import os
import statistics
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

# 4 档测试文本：取自真实日志长度分布（约 12B / 80B / 170B / 350B），含口水词
# （嗯/那个/就是）与一处热词 VoiceStick。tier1 为真实短句（12B，app 层 B3 会跳过
# 精修，本档探测模型行为与边界）。
TEST_TIERS = [
    "嗯那个好",
    "那个就是说 VoiceStick 的新功能还挺好用的嗯基本上每天都能用得到",
    "嗯那个就是关于下周的发布计划我们需要准备好 VoiceStick 的更新日志和安装包"
    "然后还要测试一下蓝牙连接的稳定性嗯大概就是这些",
    "那个就是说关于这次的用户反馈主要就是几个方面嗯首先是有几个用户反映 "
    "VoiceStick 在 Windows 上面蓝牙会断开就是连接不太稳定然后还有人说精修速度比较慢 "
    "长文本要等好几秒还有就是希望支持自定义热词表那个我建议下个版本优先修连接 "
    "然后把精修的延迟优化一下文档也要更新",
]
TEST_HOTWORDS = ["VoiceStick"]

# 逐字复刻 LLMRefinementClient.buildPrompt 默认形态（override 为空时，无热词段）。
DEFAULT_PROMPT = """你是一个语音识别后处理器。
输入为自动语音识别生成的原始文本。请将其改写为规范的书面文本：

• 去除因语音停顿产生的多余空格，尤其中文、日文、韩文字符间的空格；保留单词间及行内拉丁字母、数字周围的合法空格。

• 修正标点：按文本语言规范补全缺失标点、调整错位标点、删除冗余标点。若输入仅为短语或短词，末尾请勿添加句号。

• 剔除无实际语义的填充词、半截话、口吃及无意义口语碎片（如“嗯”“啊”“那个”“就是”“uh”“um”“you know”）。

• 保留原意、语种与语气，不翻译或扩写内容。

• 专有名词、数字、代码及专业术语保持不变。

仅返回清理后的文本，无需解释、引号、前缀、备选方案或 Markdown 格式。
"""

HOTWORDS_SUFFIX = (
    "\n\n用户常用术语热词表：{terms}。若识别文本中出现与热词发音相近的写法"
    "（如拼读、同音、分写或连写变形），请替换为热词原形；没有相近发音的词时"
    "不要凭空插入热词。示例：热词表含「AGENTS.md」时，识别文本「编辑 agentsdmd "
    "这个文件」应改为「编辑 AGENTS.md 这个文件」。原文中已经正确出现的热词必须"
    "原样保留，不得改写。"
)


def load_config() -> dict[str, str]:
    """读 macOS / Windows 本机 config.toml 的顶层简单键值（key 只进请求头）。"""
    candidates = []
    app_support = os.environ.get("HOME")
    if app_support:
        candidates.append(
            Path(app_support) / "Library" / "Application Support" / "VoiceStick" / "config.toml"
        )
    appdata = os.environ.get("APPDATA")
    if appdata:
        candidates.append(Path(appdata) / "VoiceStick" / "config.toml")
    values: dict[str, str] = {}
    for path in candidates:
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except OSError:
            continue
        for raw in lines:
            line = raw.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            value = value.strip()
            if len(value) >= 2 and value[0] == '"' and value[-1] == '"':
                value = value[1:-1].replace('\\"', '"').replace("\\\\", "\\")
            values[key.strip()] = value
        if values:
            break
    return values


def build_prompt(hotwords: list[str]) -> str:
    """复刻 buildPrompt：本探测固定用默认 prompt + 测试热词表后缀。"""
    terms = [w.strip() for w in hotwords if w.strip()]
    prompt = DEFAULT_PROMPT
    if terms:
        prompt += HOTWORDS_SUFFIX.format(terms=", ".join(terms))
    return prompt


def chat_completions_url(base_url: str) -> str:
    """复刻 chatCompletionsURL：去尾部斜杠空白，补 /chat/completions。"""
    base = base_url.strip("/ \n\r\t")
    if base.endswith("/chat/completions"):
        return base
    return base + "/chat/completions"


def make_payload(model: str, text: str, hotwords: list[str], disable_thinking: bool) -> dict:
    """复刻 makeRequest payload：stream + temperature:0 + 关思考三字段。

    与 app 保持一致**不发 max_tokens**（2026-10-14 实测：推理型模型思考链烧预算致
    content 为空；输出拖长由桌面端 B2 预算定时器兜底）。
    """
    payload: dict = {
        "model": model,
        "temperature": 0,
        "messages": [
            {"role": "system", "content": build_prompt(hotwords)},
            {"role": "user", "content": text},
        ],
        "stream": True,
    }
    if disable_thinking:
        payload["enable_thinking"] = False
        payload["chat_template_kwargs"] = {"enable_thinking": False}
        payload["think"] = False
    return payload


def stream_refine(url: str, api_key: str, payload: dict, timeout: float) -> dict:
    """SSE 流式精修一次。返回 ttft/total/块数/全文；出错抛异常由调用方记失败。"""
    body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    req = urllib.request.Request(
        url,
        data=body,
        headers={
            "Authorization": f"Bearer {api_key.strip()}",
            "Content-Type": "application/json",
        },
    )
    started = time.perf_counter()
    ttft: float | None = None
    chunks = 0
    full = ""
    raw_samples: list[str] = []  # 无 token 时归因用（原样回显，截断）
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        for raw in resp:
            line = raw.decode("utf-8", errors="replace").strip()
            if not line.startswith("data:"):
                continue
            data = line[5:].strip()
            if len(raw_samples) < 3 and data != "[DONE]":
                raw_samples.append(data[:200])
            if data == "[DONE]":
                break
            try:
                obj = json.loads(data)
            except json.JSONDecodeError:
                continue
            choices = obj.get("choices") or []
            if not choices:
                continue
            delta = choices[0].get("delta") or {}
            token = delta.get("content")
            if not token:
                continue
            chunks += 1
            full += token
            if ttft is None:
                ttft = time.perf_counter() - started
    total = time.perf_counter() - started
    if ttft is None:
        raise RuntimeError(f"stream produced no tokens; raw={raw_samples!r}")
    return {
        "ttft": ttft,
        "total": total,
        "chunks": chunks,
        "text": full.strip(),
    }


def percentile(values: list[float], pct: float) -> float:
    if not values:
        return float("nan")
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    rank = (len(ordered) - 1) * pct / 100.0
    low = int(rank)
    high = min(low + 1, len(ordered) - 1)
    frac = rank - low
    return ordered[low] * (1 - frac) + ordered[high] * frac


def fmt_ms(seconds: float) -> str:
    return f"{seconds * 1000:.0f}ms"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    cfg = load_config()
    ap.add_argument("--base-url", default=cfg.get("llm_base_url", ""))
    ap.add_argument("--model", default=cfg.get("llm_model", ""))
    ap.add_argument("--api-key", default=cfg.get("llm_api_key", ""))
    ap.add_argument("--runs", type=int, default=8, help="每档文本重复次数")
    ap.add_argument("--timeout", type=float, default=60.0, help="单请求超时秒数")
    ap.add_argument(
        "--thinking",
        action="store_true",
        help="覆盖 config 的 llm_disable_thinking=false（默认按 config，关思考字段照常发）",
    )
    args = ap.parse_args()

    if not (args.base_url and args.model):
        print("缺少 base-url/model（config.toml 无 llm_base_url/llm_model）", file=sys.stderr)
        return 1
    api_key = (args.api_key or "").strip() or "ollama"  # Ollama 忽略该头，占位即可
    disable_thinking = not args.thinking and cfg.get("llm_disable_thinking", "true") != "false"
    url = chat_completions_url(args.base_url)

    print(f"endpoint={url}")
    print(f"model={args.model} runs={args.runs} thinking={'off' if disable_thinking else 'on'}")
    print("=" * 78)

    all_pass = True
    for tier_text in TEST_TIERS:
        nbytes = len(tier_text.encode("utf-8"))
        payload = make_payload(args.model, tier_text, TEST_HOTWORDS, disable_thinking)
        ttfts: list[float] = []
        totals: list[float] = []
        chunk_counts: list[int] = []
        gen_seconds: list[float] = []
        failures = 0
        results: list[str] = []
        hotword_miss = 0
        content_suspect = 0
        for run in range(args.runs):
            try:
                r = stream_refine(url, api_key, payload, args.timeout)
            except (urllib.error.URLError, TimeoutError, RuntimeError, OSError) as e:
                failures += 1
                print(f"  [tier {nbytes}B run {run + 1}] ERROR: {e}")
                continue
            ttfts.append(r["ttft"])
            totals.append(r["total"])
            chunk_counts.append(r["chunks"])
            gen_seconds.append(max(r["total"] - r["ttft"], 1e-6))
            refined = r["text"]
            results.append(refined)
            if any(hw in tier_text and hw not in refined for hw in TEST_HOTWORDS):
                hotword_miss += 1
            # 丢内容启发式：结果短于原文 50% 视为可疑（口水词剔除的正常压缩约 60-90%）。
            if refined and len(refined) < len(tier_text) * 0.5:
                content_suspect += 1

        n = len(ttfts)
        toks_per_s = (
            statistics.mean(c / g for c, g in zip(chunk_counts, gen_seconds)) if gen_seconds else float("nan")
        )
        ok = failures == 0 and hotword_miss == 0
        all_pass = all_pass and ok
        print(
            f"tier {nbytes:>3}B  n={n}/{args.runs}  fail={failures}  "
            f"TTFT p50={fmt_ms(percentile(ttfts, 50))} p95={fmt_ms(percentile(ttfts, 95))}  "
            f"total p50={fmt_ms(percentile(totals, 50))} p95={fmt_ms(percentile(totals, 95))}  "
            f"tok/s≈{toks_per_s:.1f}  hotword_miss={hotword_miss}  content_suspect={content_suspect}"
        )
        for run, refined in enumerate(results):
            print(f"  --- run {run + 1} ---")
            print(f"  原文: {tier_text}")
            print(f"  精修: {refined}")
        print("=" * 78)

    print("overall: OK" if all_pass else "overall: ISSUES (fail/hotword_miss)")
    return 0 if all_pass else 1


if __name__ == "__main__":
    sys.exit(main())
