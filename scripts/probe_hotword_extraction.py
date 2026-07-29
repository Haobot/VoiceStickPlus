#!/usr/bin/env python3
"""离线探测 LLM 热词提炼链路：复刻 Windows 端 BuildHotwordExtractionPrompt /
ParseHotwordExtractionResponse 的行为，直接打印模型原始响应与每个候选被哪条
过滤规则拒掉，用于定位「hotword extraction finished ok=1 candidates=0」。

用法:
    python scripts/probe_hotword_extraction.py [--text TEXT ...] [--repeat N]

凭据从 %APPDATA%\\VoiceStick\\config.toml 读取，API key 只进请求头，绝不打印。
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import tomllib
import urllib.request

DEFAULT_TEXTS = [
    "我刚才讲了 Stack Chain 这个新词",
    "Stack Chain",
    "测试一下 agents dmd 和 cloud dmd",
    "今天天气不错,我们出去走走吧",
    "我用 DeepSeek 和 Kimi 写代码",
]


def load_config() -> dict:
    path = os.path.join(os.environ["APPDATA"], "VoiceStick", "config.toml")
    with open(path, "rb") as f:
        return tomllib.load(f)


def build_prompt(hotwords: list[str]) -> str:
    """逐字复刻 LLMRefinementClient::BuildHotwordExtractionPrompt。"""
    prompt = (
        "你是热词候选提取器。从语音识别结果文本中提取可能是专有名词的词或短语:"
        "产品名、项目名、公司名、技术术语、文件名、代号等。\n"
        "规则:\n"
        "• 只提取文本中实际出现的内容,严格保留原始拼写与大小写,不臆造、不翻译、不纠错。\n"
        "• 普通词汇、常见英文单词、人称代词与完整句子不要提取。\n"
        "• 文本中非句首的首字母大写词或词组,即使看起来像普通英文单词的组合,也应作为候选输出。\n"
        "• 已知热词表中的条目不要重复提取。\n"
        "• 最多输出 5 个。\n"
        '• 只输出 JSON 数组(如 ["DeepSeek", "Stack Chain"]),没有候选时输出 [];'
        "不要输出解释或其他任何内容。\n"
        '示例:输入「我刚才讲了 Stack Chain 这个新词」→ 输出 ["Stack Chain"];'
        "输入「今天天气不错,我们出去走走吧」→ 输出 []。"
    )
    if not hotwords:
        return prompt
    prompt += "\n已知热词表:" + ", ".join(hotwords) + "。"
    return prompt


def chat(base_url: str, api_key: str, model: str, system_prompt: str, user_text: str) -> str:
    url = base_url.rstrip("/")
    if not url.endswith("/chat/completions"):
        url += "/chat/completions"
    if not url.startswith(("http://", "https://")):
        url = "https://" + url
    payload = json.dumps(
        {
            "model": model,
            "temperature": 0,
            "messages": [
                {"role": "system", "content": system_prompt},
                {"role": "user", "content": user_text},
            ],
        }
    ).encode("utf-8")
    req = urllib.request.Request(
        url,
        data=payload,
        headers={
            "Authorization": f"Bearer {api_key.strip()}",
            "Content-Type": "application/json",
        },
    )
    with urllib.request.urlopen(req, timeout=60) as resp:
        body = json.loads(resp.read().decode("utf-8"))
    return body["choices"][0]["message"]["content"].strip()


def parse_with_reasons(response: str, source_text: str, hotwords: list[str]):
    """复刻 ParseHotwordExtractionResponse,并输出每条候选的拒绝原因。"""
    start = response.find("[")
    end = response.rfind("]")
    if start == -1 or end == -1 or end <= start:
        return [], [f"no bracket pair found in response (len={len(response)})"]
    try:
        items = json.loads(response[start : end + 1])
    except json.JSONDecodeError as e:
        return [], [f"json parse failed: {e}"]
    if not isinstance(items, list):
        return [], ["json root is not an array"]

    accepted, rejected = [], []
    seen_lower = set()
    for item in items:
        if not isinstance(item, str):
            rejected.append(f"{item!r}: not a string")
            continue
        word = item.strip()
        if len(word) < 2 or len(word) > 40:
            rejected.append(f"{word!r}: length {len(word)} out of 2..40")
            continue
        if word.count(" ") + 1 > 3:
            rejected.append(f"{word!r}: more than 3 words")
            continue
        if word.lower() not in source_text.lower():
            rejected.append(f"{word!r}: not found in source text")
            continue
        if any(word.lower() == h.lower() for h in hotwords):
            rejected.append(f"{word!r}: already in hotword list")
            continue
        if word.lower() in seen_lower:
            rejected.append(f"{word!r}: duplicate")
            continue
        seen_lower.add(word.lower())
        accepted.append(word)
    return accepted, rejected


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--text", action="append", dest="texts", help="自定义测试文本,可多次")
    ap.add_argument("--repeat", type=int, default=1, help="每条文本重复调用次数(观察稳定性)")
    args = ap.parse_args()

    cfg = load_config()
    base_url = cfg.get("llm_base_url", "")
    api_key = cfg.get("llm_api_key", "")
    model = cfg.get("llm_model", "")
    hotwords = cfg.get("asr_hotwords", [])
    if isinstance(hotwords, str):
        hotwords = [w.strip() for w in re.split(r"[,，]", hotwords) if w.strip()]
    if not (base_url and api_key and model):
        print("config.toml 缺少 llm_base_url / llm_api_key / llm_model", file=sys.stderr)
        return 1

    prompt = build_prompt(hotwords)
    print(f"model={model} base_url={base_url} hotwords={len(hotwords)} 条")
    print("=" * 70)

    texts = args.texts or DEFAULT_TEXTS
    for text in texts:
        for round_i in range(args.repeat):
            suffix = f" (round {round_i + 1})" if args.repeat > 1 else ""
            print(f"\n>>> {text}{suffix}")
            try:
                response = chat(base_url, api_key, model, prompt, text)
            except Exception as e:  # noqa: BLE001 - 探测脚本直接暴露错误
                print(f"  HTTP/网络错误: {e}")
                continue
            print(f"  原始响应: {response!r}")
            accepted, rejected = parse_with_reasons(response, text, hotwords)
            print(f"  通过: {accepted}")
            for reason in rejected:
                print(f"  拒绝: {reason}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
