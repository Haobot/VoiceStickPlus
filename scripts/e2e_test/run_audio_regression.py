#!/usr/bin/env python3
"""音频资源库自动化回归测试（语音合成回归闭环）。

对音频资源库（corpus/ 清单驱动的 .pcm/.ogg/.txt 三件套）执行闭环回归：
  1. 资源库完整性校验：按清单逐条核对产物存在性与格式（OggS/OpusHead、
     s16le PCM、时长区间、非静音、文本一致），确保所有音频可被正确识别和调用
  2. 合成回归：对每条语料用 edge-tts 后台依次重新合成（临时目录，不污染资源库），
     逐条捕获异常与超时，确保合成过程无异常中断
  3. 质量检测：对合成产物按预设标准判定（格式/时长/响度/文本一致）
  4. 报告：全部通过打印「语音通过，全通过」；存在未通过项时逐条列出原因与阶段，
     报告落盘 bench_results/audio_regression_<stamp>.json|.md
  5. 跟踪闭环：bench_results/audio_regression_state.json 标记失败项
     （新增失败/持续失败/本轮修复三分类），持续跟踪直至全部通过

触发方式：
  手动一次    python run_audio_regression.py
  监听模式    python run_audio_regression.py --watch        # 源码变更自动触发
  git 钩子    python run_audio_regression.py --install-hook # 每次 commit 后自动触发
  离线快检    python run_audio_regression.py --offline      # 只校验资源库，不联网合成
  查看跟踪    python run_audio_regression.py --list-failures

退出码 0 = 全部通过（--allow-known-fail 时仅持续失败也视为通过）；
1 = 有失败项；2 = 环境缺失（ffmpeg/edge-tts/清单不存在，不伪造结果）。
依赖：edge-tts（--offline 模式不需要）、ffmpeg（PATH 或 --ffmpeg）。
"""
from __future__ import annotations

import argparse
import asyncio
import json
import math
import re
import shutil
import subprocess
import sys
import tempfile
import time
import zlib
from array import array
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
CORPUS_DIR = HERE / "corpus"
DEFAULT_MANIFEST = CORPUS_DIR / "corpus.json"
DEFAULT_OUT = HERE / "bench_results"
STATE_FILE = DEFAULT_OUT / "audio_regression_state.json"

# ---- 预设质量标准 ----
MIN_PCM_BYTES = 16000          # 0.5s @ 16kHz/16bit/mono（对齐固件 <0.5s 丢弃阈值）
MAX_DURATION_S = 120.0         # 单条语料时长上限
MIN_RMS = 100                  # s16le RMS 下限（约 -50 dBFS），过滤静音/损坏输出
UNIT_MIN_CPS = 12.0            # 语速下界：每语音单元最快 12 单元/秒（超出判为异常）
UNIT_MAX_CPS = 1.2             # 语速上界：每语音单元最慢 1.2 单元/秒
MIN_ABS_DURATION = 0.5         # 时长绝对下限（秒）
MAX_ABS_DURATION = 3.0         # 短文本时长上限保护（秒）

# ---- 触发：监听与钩子 ----
DEFAULT_WATCH_ROOTS = [
    "firmware", "desktop/windows", "desktop/macos", "website/src", "scripts/e2e_test",
]
WATCH_EXCLUDE_DIRS = {
    "build", "build-x64", "node_modules", ".git", "bench_results", "battery_logs",
    "power_logs", "generated_winrt", "__pycache__", ".trae", "managed_components",
}
WATCH_EXCLUDE_SUFFIX = (".log", ".tmp", ".pyc", ".bin")
WATCH_POLL_INTERVAL = 2.0
WATCH_DEBOUNCE = 3.0
HOOK_BEGIN = "# >>> voicestick-audio-regression >>>"
HOOK_END = "# <<< voicestick-audio-regression <<<"

_PCM_RE = re.compile(r"[^\w]+", re.UNICODE)


def log(msg: str) -> None:
    print(f"[audio-regression] {msg}", flush=True)


def rate_factor(rate: str | None) -> float:
    """edge-tts 变速参数转倍率系数："+40%" -> 1.4，"-20%" -> 0.8。"""
    if not rate:
        return 1.0
    m = re.fullmatch(r"([+-]?\d+(?:\.\d+)?)%", rate.strip())
    if not m:
        return 1.0
    return 1.0 + float(m.group(1)) / 100.0


def speech_units(text: str) -> float:
    """估算语音单元数：CJK 字符按字计，连续拉丁字母/数字按词计。"""
    units = 0.0
    buf = ""
    for ch in text:
        if re.match(r"[\u4e00-\u9fff\u3000-\u303f\uff00-\uffef]", ch):
            if buf:
                units += 1.0
                buf = ""
            if re.match(r"[\u4e00-\u9fff]", ch):
                units += 1.0  # 汉字计数；中文标点不计
        elif ch.isalnum():
            buf += ch
        else:
            if buf:
                units += 1.0
                buf = ""
    if buf:
        units += 1.0
    return max(1.0, units)


def duration_bounds(text: str, rate: str | None) -> tuple[float, float]:
    """按文本量与语速推算合理时长区间（秒）。"""
    f = max(0.2, rate_factor(rate))
    units = speech_units(text)
    lo = max(MIN_ABS_DURATION, units / UNIT_MIN_CPS / f)
    hi = max(MAX_ABS_DURATION, units / UNIT_MAX_CPS / f)
    return lo, hi


def rms_s16le(pcm_path: Path) -> float:
    """raw s16le PCM 的 RMS（整文件跨步采样，防大文件拖慢）。"""
    raw = pcm_path.read_bytes()
    samples = array("h")
    samples.frombytes(raw[: (len(raw) // 2) * 2])
    if not len(samples):
        return 0.0
    step = max(1, len(samples) // 500_000)
    total = 0.0
    n = 0
    for i in range(0, len(samples), step):
        v = samples[i]
        total += v * v
        n += 1
    return math.sqrt(total / max(1, n))


def check_audio_pair(pcm: Path, ogg: Path, text: str, rate: str | None) -> list[str]:
    """校验一对 pcm/ogg 产物，返回失败原因列表（空 = 通过）。"""
    failures: list[str] = []

    if not pcm.exists():
        failures.append(f"缺少 {pcm.name}")
    else:
        size = pcm.stat().st_size
        dur = size / 32000.0
        lo, hi = duration_bounds(text, rate)
        if size < MIN_PCM_BYTES:
            failures.append(f"pcm 过短 {size}B（{dur:.2f}s < {MIN_ABS_DURATION}s 下限）")
        elif size % 2 != 0:
            failures.append("pcm 字节数非偶数，非 s16le")
        elif not (lo <= dur <= hi):
            failures.append(f"时长 {dur:.2f}s 超出合理区间 [{lo:.2f}, {hi:.2f}]s（文本量/语速推算）")
        elif dur > MAX_DURATION_S:
            failures.append(f"时长 {dur:.2f}s 超过单条上限 {MAX_DURATION_S}s")
        else:
            r = rms_s16le(pcm)
            if r < MIN_RMS:
                failures.append(f"疑似静音/损坏：RMS {r:.0f} < {MIN_RMS}")

    if not ogg.exists():
        failures.append(f"缺少 {ogg.name}")
    else:
        data = ogg.read_bytes()
        if data[:4] != b"OggS":
            failures.append("ogg magic 非 OggS")
        if b"OpusHead" not in data[:1024]:
            failures.append("ogg 首 1KB 未找到 OpusHead")
        if len(data) == 0:
            failures.append("ogg 为空")

    return failures


def check_library_item(item: dict, corpus_dir: Path) -> list[str]:
    """资源库完整性：核对三件套齐全、格式正确、文本与清单一致。"""
    cid = item["id"]
    failures: list[str] = []
    pcm = corpus_dir / f"{cid}.pcm"
    ogg = corpus_dir / f"{cid}.ogg"
    txt = corpus_dir / f"{cid}.txt"

    for p in (pcm, ogg, txt):
        if not p.exists():
            failures.append(f"资源库缺少产物 {p.name}")
    if failures:
        return failures

    failures.extend(check_audio_pair(pcm, ogg, item["text"], item.get("rate")))

    content = txt.read_text(encoding="utf-8").strip()
    if not content:
        failures.append("txt 为空")
    elif content != item["text"]:
        failures.append(f"txt 与清单不一致：期望「{item['text']}」实际「{content}」")
    return failures


async def synth_one(item: dict, default_voice: str, ffmpeg: str, tmpdir: Path,
                    timeout: float) -> list[str]:
    """用 edge-tts 重新合成单条语料到临时目录并质检。返回失败原因列表。"""
    import edge_tts  # 惰性导入：--offline 模式不要求安装

    cid = item["id"]
    text = item["text"]
    voice = item.get("voice") or default_voice
    kwargs = {}
    if item.get("rate"):
        kwargs["rate"] = item["rate"]

    mp3 = tmpdir / f"{cid}.mp3"
    pcm = tmpdir / f"{cid}.pcm"
    ogg = tmpdir / f"{cid}.ogg"

    communicate = edge_tts.Communicate(text, voice, **kwargs)
    try:
        await asyncio.wait_for(communicate.save(str(mp3)), timeout=timeout)
    except asyncio.TimeoutError:
        return [f"合成中断：edge-tts 超时（>{timeout:.0f}s）"]
    except Exception as e:  # noqa: BLE001 - 逐条收集失败不中断整体
        return [f"合成中断：{type(e).__name__}: {e}"]

    if not mp3.exists() or mp3.stat().st_size == 0:
        return ["合成中断：edge-tts 输出空 mp3"]

    try:
        subprocess.run(
            [ffmpeg, "-y", "-i", str(mp3), "-ar", "16000", "-ac", "1",
             "-f", "s16le", str(pcm)],
            check=True, capture_output=True, timeout=timeout,
        )
        subprocess.run(
            [ffmpeg, "-y", "-i", str(mp3), "-ar", "16000", "-ac", "1",
             "-c:a", "libopus", "-b:a", "32k", "-application", "voip", str(ogg)],
            check=True, capture_output=True, timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return [f"合成中断：ffmpeg 转码超时（>{timeout:.0f}s）"]
    except subprocess.CalledProcessError as e:
        tail = (e.stderr or b"")[-200:].decode("utf-8", "replace").strip()
        return [f"合成中断：ffmpeg 转码失败 {tail}"]

    return check_audio_pair(pcm, ogg, text, item.get("rate"))


def find_ffmpeg(explicit: str | None) -> str | None:
    if explicit:
        p = Path(explicit)
        if p.is_file():
            return str(p)
        log(f"WARN: --ffmpeg 指定路径不存在 {explicit}")
    return shutil.which("ffmpeg")


def load_manifest(path: Path) -> list[dict]:
    items = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(items, list) or not items:
        raise ValueError(f"清单为空或格式错误：{path}")
    for it in items:
        if "id" not in it or "text" not in it:
            raise ValueError(f"清单条目缺少 id/text：{it}")
    return items


# ---------------- 状态跟踪 ----------------

def load_state() -> dict:
    if STATE_FILE.exists():
        try:
            state = json.loads(STATE_FILE.read_text(encoding="utf-8"))
            if isinstance(state.get("manifests"), dict):
                return state
        except Exception:  # noqa: BLE001 - 损坏则重建
            log("WARN: 状态文件损坏，重建")
    return {"manifests": {}}


def save_state(state: dict) -> None:
    DEFAULT_OUT.mkdir(parents=True, exist_ok=True)
    state["updated_at"] = datetime.now().isoformat(timespec="seconds")
    STATE_FILE.write_text(json.dumps(state, ensure_ascii=False, indent=2),
                          encoding="utf-8")


def update_tracking(state: dict, manifest_key: str,
                    results: dict[str, dict], now: str) -> dict[str, str]:
    """按清单分域更新逐项跟踪状态，返回分类映射：fixed / new_fail / still_fail / pass。"""
    scope = state["manifests"].setdefault(manifest_key, {"items": {}})
    items_state: dict = scope.setdefault("items", {})
    classification: dict[str, str] = {}
    for cid, r in results.items():
        prev = items_state.get(cid, {})
        if r["status"] == "pass":
            classification[cid] = "fixed" if prev.get("status") == "fail" else "pass"
            items_state[cid] = {
                "status": "pass", "fail_streak": 0,
                "last_pass": now,
                "last_fail": prev.get("last_fail"), "first_fail": None,
                "last_reasons": [],
            }
        else:
            streak = int(prev.get("fail_streak") or 0) + 1
            classification[cid] = ("still_fail" if prev.get("status") == "fail"
                                   else "new_fail")
            items_state[cid] = {
                "status": "fail", "fail_streak": streak,
                "first_fail": prev.get("first_fail") or now,
                "last_fail": now, "last_reasons": r["reasons"],
            }
    return classification


# ---------------- 报告 ----------------

CLS_LABEL = {"new_fail": "新增失败", "still_fail": "持续失败", "fixed": "本轮修复",
             "pass": "通过"}


def write_reports(out_dir: Path, report: dict) -> tuple[Path, Path]:
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = report["stamp"]
    jpath = out_dir / f"audio_regression_{stamp}.json"
    mpath = out_dir / f"audio_regression_{stamp}.md"
    seq = 1
    while jpath.exists():  # 同秒多次运行时避免互相覆盖
        seq += 1
        jpath = out_dir / f"audio_regression_{stamp}-{seq}.json"
        mpath = out_dir / f"audio_regression_{stamp}-{seq}.md"
    jpath.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")

    lines = [
        "# 音频回归测试报告",
        "",
        f"- 时间：{report['time']}  触发：{report['trigger']}  模式：{report['mode']}",
        f"- 清单：`{report['manifest']}`",
        f"- 结果：语料 {report['total']} | 通过 {report['passed']} | "
        f"失败 {report['failed']}"
        f"（新增 {report['new_fail']} / 持续 {report['still_fail']} / 修复 {report['fixed']}）"
        f"  耗时 {report['elapsed_s']:.1f}s",
        f"- **结论：{'语音通过，全通过' if report['all_pass'] else '存在未通过项，需排查'}**",
        "",
    ]
    fails = [it for it in report["items"] if it["status"] == "fail"]
    if fails:
        lines += ["## 未通过项", "",
                  "| id | category | 分类 | 阶段 | 失败原因 | 连续失败 | 首次失败 |",
                  "|---|---|---|---|---|---|---|"]
        for it in fails:
            reasons = "；".join(it["reasons"]).replace("|", "\\|")
            lines.append(
                f"| {it['id']} | {it['category']} | {CLS_LABEL[it['classification']]} "
                f"| {it['stage']} | {reasons} | {it['fail_streak']} "
                f"| {it['first_fail'] or '-'} |")
        lines += ["",
                  "未通过项已标记跟踪（`bench_results/audio_regression_state.json`），"
                  "修复后重跑本脚本直至全部通过。"]
    else:
        lines += ["全部语料合成与质量检测通过，无跟踪中的失败项。"]
    mpath.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return jpath, mpath


def print_summary(report: dict, state_path: Path) -> None:
    total, failed = report["total"], report["failed"]
    if report["all_pass"]:
        log(f"语音通过，全通过（{report['passed']}/{total}，{report['mode']}，"
            f"耗时 {report['elapsed_s']:.1f}s）")
        if report["fixed"]:
            log(f"本轮修复 {report['fixed']} 项："
                + ", ".join(it["id"] for it in report["items"]
                            if it["classification"] == "fixed"))
        return

    log(f"音频回归测试未全部通过：{failed}/{total} 项失败")
    for it in report["items"]:
        if it["status"] != "fail":
            continue
        log(f"  [{CLS_LABEL[it['classification']]}] {it['id']}"
            f"（{it['category'] or '-'}，{it['stage']}）："
            + "；".join(it["reasons"]))
    fixed = [it["id"] for it in report["items"] if it["classification"] == "fixed"]
    if fixed:
        log(f"  [本轮修复] {', '.join(fixed)}")
    log("请对以上未通过项进行原因排查；失败项已标记跟踪，修复后重跑本脚本验证闭环。")
    log(f"跟踪状态文件：{state_path}")


# ---------------- 主流程 ----------------

async def run_regression(args: argparse.Namespace) -> int:
    t0 = time.monotonic()
    manifest_path = Path(args.manifest)
    corpus_dir = manifest_path.resolve().parent
    out_dir = Path(args.out)

    # 预检：环境缺失直接退出（不伪造结果，不污染跟踪状态）
    if not manifest_path.exists():
        log(f"FAIL: 清单不存在 {manifest_path}")
        return 2
    try:
        items = load_manifest(manifest_path)
    except Exception as e:  # noqa: BLE001
        log(f"FAIL: {e}")
        return 2
    if args.only:
        only = {x.strip() for x in args.only.split(",") if x.strip()}
        items = [it for it in items if it["id"] in only]
        if not items:
            log(f"FAIL: --only 未匹配到任何语料：{args.only}")
            return 2

    ffmpeg = find_ffmpeg(args.ffmpeg)
    do_synth = not args.offline
    if do_synth:
        if not ffmpeg:
            ffmpeg = find_ffmpeg(None)
        if not ffmpeg:
            log("FAIL: 未找到 ffmpeg（--ffmpeg PATH 或加入 PATH）")
            return 2
        try:
            import edge_tts  # noqa: F401
        except ImportError:
            log("FAIL: 未安装 edge-tts（pip install edge-tts），或改用 --offline 离线快检")
            return 2

    mode = "离线校验" if args.offline else "合成回归"
    log(f"开始音频回归：{mode}，清单 {manifest_path.name}，共 {len(items)} 条")

    # 阶段 A：资源库完整性（确保所有音频可被正确识别和调用）
    results: dict[str, dict] = {}
    for it in items:
        cid = it["id"]
        lib_fail = check_library_item(it, corpus_dir)
        results[cid] = {"id": cid, "category": it.get("category", ""),
                        "status": "pass", "stage": "-", "reasons": [],
                        "lib_fail": lib_fail}

    # 阶段 B+C：后台依次合成 + 质量检测（临时目录，不污染资源库）
    if do_synth:
        with tempfile.TemporaryDirectory(prefix="vs_audio_reg_") as td:
            tmpdir = Path(td)
            for idx, it in enumerate(items, 1):
                cid = it["id"]
                log(f"  [{idx}/{len(items)}] 合成测试 {cid} ...")
                reasons = await synth_one(it, args.voice, ffmpeg, tmpdir,
                                          args.synth_timeout)
                results[cid]["synth_fail"] = reasons
    else:
        for r in results.values():
            r["synth_fail"] = []

    # 汇总逐项状态：资源库与合成两阶段都通过才算通过
    for r in results.values():
        if r["lib_fail"]:
            r["status"] = "fail"
            r["stage"] = "资源库"
            r["reasons"] = r["lib_fail"]
        elif r["synth_fail"]:
            r["status"] = "fail"
            r["stage"] = "合成/质检"
            r["reasons"] = r["synth_fail"]

    # 跟踪闭环：更新状态文件并分类（按清单路径分域，互不污染）
    now = datetime.now().isoformat(timespec="seconds")
    state = load_state()
    manifest_key = str(manifest_path.resolve())
    classification = update_tracking(state, manifest_key, results, now)
    scope_items = state["manifests"][manifest_key]["items"]
    for r in results.values():
        r["classification"] = classification[r["id"]]
        r["fail_streak"] = scope_items[r["id"]].get("fail_streak", 0)
        r["first_fail"] = scope_items[r["id"]].get("first_fail")
    state["last_run"] = {
        "time": now, "mode": mode, "trigger": args.trigger,
        "manifest": str(manifest_path), "total": len(items),
        "passed": sum(1 for r in results.values() if r["status"] == "pass"),
        "failed": sum(1 for r in results.values() if r["status"] == "fail"),
        "all_pass": all(r["status"] == "pass" for r in results.values()),
    }
    save_state(state)

    n_fail = sum(1 for r in results.values() if r["status"] == "fail")
    report = {
        "stamp": datetime.now().strftime("%Y%m%d-%H%M%S"),
        "time": now, "trigger": args.trigger, "mode": mode,
        "manifest": str(manifest_path), "total": len(items),
        "passed": len(items) - n_fail, "failed": n_fail,
        "new_fail": sum(1 for r in results.values() if r["classification"] == "new_fail"),
        "still_fail": sum(1 for r in results.values() if r["classification"] == "still_fail"),
        "fixed": sum(1 for r in results.values() if r["classification"] == "fixed"),
        "all_pass": n_fail == 0,
        "elapsed_s": time.monotonic() - t0,
        "items": [
            {"id": r["id"], "category": r["category"], "status": r["status"],
             "stage": r["stage"], "classification": r["classification"],
             "reasons": r["reasons"], "fail_streak": r["fail_streak"],
             "first_fail": r["first_fail"]}
            for r in results.values()
        ],
    }
    jpath, mpath = write_reports(out_dir, report)
    print_summary(report, STATE_FILE)
    log(f"报告：{jpath.name} / {mpath.name}")

    if report["all_pass"]:
        return 0
    if args.allow_known_fail and report["new_fail"] == 0:
        log("--allow-known-fail：仅有跟踪中的持续失败项，按通过处理")
        return 0
    return 1


# ---------------- 触发机制：监听 ----------------

def dir_signature(root: Path) -> int:
    sig = 0
    for p in root.rglob("*"):
        if p.is_dir():
            continue
        if any(part in WATCH_EXCLUDE_DIRS for part in p.parts):
            continue
        if p.name.endswith(WATCH_EXCLUDE_SUFFIX) or p.name == ".idf_cli_state.json":
            continue
        try:
            st = p.stat()
        except OSError:
            continue
        sig = (sig + zlib.crc32(str(p.relative_to(root)).encode())
               ^ (st.st_mtime_ns & 0xFFFFFFFF) ^ st.st_size) & 0xFFFFFFFFFFFFFFFF
    return sig


def watch_loop(args: argparse.Namespace) -> int:
    repo = HERE.parent.parent
    roots = [repo / r for r in (args.watch_roots or DEFAULT_WATCH_ROOTS)]
    roots = [r for r in roots if r.exists()]
    if not roots:
        log("FAIL: 监听目录均不存在")
        return 2
    log(f"监听模式：变更后自动触发回归（轮询 {WATCH_POLL_INTERVAL:.0f}s，"
        f"防抖 {WATCH_DEBOUNCE:.0f}s）；Ctrl+C 退出")
    log("监听目录：" + ", ".join(str(r.relative_to(repo)) for r in roots))

    baseline = {r: dir_signature(r) for r in roots}
    try:
        while True:
            time.sleep(WATCH_POLL_INTERVAL)
            changed = [r for r in roots if dir_signature(r) != baseline[r]]
            if not changed:
                continue
            log(f"检测到代码变更：{', '.join(r.name for r in changed)}，"
                f"等待 {WATCH_DEBOUNCE:.0f}s 确认稳定 ...")
            deadline = time.monotonic() + 30.0
            while time.monotonic() < deadline:
                time.sleep(WATCH_DEBOUNCE)
                if all(dir_signature(r) == baseline[r] for r in roots):
                    break
                baseline = {r: dir_signature(r) for r in roots}
            args.trigger = "watch"
            asyncio.run(run_regression(args))
            baseline = {r: dir_signature(r) for r in roots}
            log("继续监听 ...")
    except KeyboardInterrupt:
        log("监听模式退出")
        return 0


# ---------------- 触发机制：git 钩子 ----------------

def git_repo_root() -> Path | None:
    try:
        out = subprocess.run(["git", "rev-parse", "--show-toplevel"],
                             capture_output=True, text=True, check=True,
                             cwd=HERE).stdout.strip()
        return Path(out)
    except Exception:  # noqa: BLE001
        return None


def install_hook() -> int:
    repo = git_repo_root()
    if not repo:
        log("FAIL: 未找到 git 仓库根目录")
        return 2
    hooks = repo / ".git" / "hooks"
    hooks.mkdir(parents=True, exist_ok=True)
    hook = hooks / "post-commit"
    block = (
        f"{HOOK_BEGIN}\n"
        "# 音频回归测试：commit 后后台执行，日志见 bench_results/hook_trigger.log\n"
        "( cd \"$(git rev-parse --show-toplevel)\" && "
        "mkdir -p scripts/e2e_test/bench_results && "
        f"\"{sys.executable}\" scripts/e2e_test/run_audio_regression.py --trigger hook "
        ">> scripts/e2e_test/bench_results/hook_trigger.log 2>&1 ) &\n"
        f"{HOOK_END}\n"
    )
    existing = hook.read_text(encoding="utf-8") if hook.exists() else "#!/bin/sh\n"
    if HOOK_BEGIN in existing:
        log("post-commit 钩子已安装，跳过")
        return 0
    content = existing.rstrip("\n") + "\n\n" + block
    hook.write_text(content, encoding="utf-8")
    import os
    import stat as stat_mod
    os.chmod(hook, os.stat(hook).st_mode | stat_mod.S_IXUSR | stat_mod.S_IXGRP
             | stat_mod.S_IXOTH)
    log(f"已安装 post-commit 钩子：{hook}")
    log("每次 commit 后自动在后台执行音频回归，日志：bench_results/hook_trigger.log")
    return 0


def uninstall_hook() -> int:
    repo = git_repo_root()
    if not repo:
        return 2
    hook = repo / ".git" / "hooks" / "post-commit"
    if not hook.exists():
        log("post-commit 钩子不存在，无需卸载")
        return 0
    lines = hook.read_text(encoding="utf-8").splitlines(keepends=True)
    out, inside = [], False
    for ln in lines:
        if ln.startswith(HOOK_BEGIN):
            inside = True
            continue
        if ln.startswith(HOOK_END):
            inside = False
            continue
        if not inside:
            out.append(ln)
    hook.write_text("".join(out), encoding="utf-8")
    log(f"已卸载 post-commit 钩子：{hook}")
    return 0


def list_failures() -> int:
    state = load_state()
    manifests = {k: v for k, v in state.get("manifests", {}).items()
                 if any(it.get("status") == "fail"
                        for it in v.get("items", {}).values())}
    if not manifests:
        print("当前无跟踪中的失败项（全部通过）。")
        return 0
    for manifest_key, scope in manifests.items():
        fails = {cid: v for cid, v in scope["items"].items()
                 if v.get("status") == "fail"}
        print(f"清单 {manifest_key} 跟踪中的失败项 {len(fails)} 条：")
        for cid, v in fails.items():
            reasons = "；".join(v.get("last_reasons") or [])
            print(f"  - {cid}（连续失败 {v.get('fail_streak', '?')} 次，"
                  f"首次 {v.get('first_fail', '?')}）：{reasons}")
    return 1


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except Exception:  # noqa: BLE001
        pass
    ap = argparse.ArgumentParser(
        description="音频资源库自动化回归测试（语音合成回归闭环）",
        formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("--manifest", default=str(DEFAULT_MANIFEST),
                    help=f"语料清单路径（默认 {DEFAULT_MANIFEST}），"
                         "可指向 corpus/hotword_corpus.json")
    ap.add_argument("--only", default="", help="只跑指定语料，逗号分隔 id")
    ap.add_argument("--voice", default="zh-CN-XiaoxiaoNeural", help="默认 edge-tts 音色")
    ap.add_argument("--ffmpeg", default=None, help="ffmpeg 路径（默认搜 PATH）")
    ap.add_argument("--synth-timeout", type=float, default=60.0,
                    help="单条合成/转码超时秒数（默认 60）")
    ap.add_argument("--out", default=str(DEFAULT_OUT), help="报告输出目录")
    ap.add_argument("--offline", action="store_true",
                    help="离线快检：只校验资源库现有产物，不做合成")
    ap.add_argument("--allow-known-fail", action="store_true",
                    help="仅有持续失败（已跟踪）项时退出码按 0 处理")
    ap.add_argument("--trigger", default="manual", choices=["manual", "watch", "hook"],
                    help=argparse.SUPPRESS)
    ap.add_argument("--watch", nargs="*", default=None, metavar="DIR",
                    help="监听模式：源码变更自动触发（默认监听 firmware/desktop/website/src 等）")
    ap.add_argument("--install-hook", action="store_true",
                    help="安装 git post-commit 钩子（commit 后自动触发回归）")
    ap.add_argument("--uninstall-hook", action="store_true", help="卸载 post-commit 钩子")
    ap.add_argument("--list-failures", action="store_true",
                    help="只列出跟踪中的失败项，不执行测试")
    args = ap.parse_args()

    if args.install_hook:
        return install_hook()
    if args.uninstall_hook:
        return uninstall_hook()
    if args.list_failures:
        return list_failures()
    if args.watch is not None:
        args.watch_roots = args.watch
        return watch_loop(args)
    return asyncio.run(run_regression(args))


if __name__ == "__main__":
    sys.exit(main())
