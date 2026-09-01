#!/usr/bin/env python3
"""小米遥控器 ATVV golden 会话的 ASR 离线评测（闭环管道）。

输入为 atvv_capture.py 采集的 fixtures 目录（session_<N>.adpcm 原始流 +
session_<N>.json sidecar），按 sidecar 段落复现解码为 16kHz PCM，做与桌面端
同参数的后处理（三点平滑 + 增益，默认 12dB 对齐 XiaomiAtvvSession::Options；
粒度差异：本脚本按整段会话平滑，桌面端按 640 样本帧切片平滑，每 640 样本
有 2 个边界样本取值不同，约 0.3%，对 ASR 无实质影响），
再以裸 PCM 直送真实 ASR（火山 bigmodel_async format=pcm / 腾讯 voice_format=1；
Python 侧无 Opus 编码器，PCM 直送是协议允许的等价路径），按 run_asr_bench.py
既有口径输出 CER/延迟报告到 bench_results/（atvv_bench_<stamp>.json|.md）。

用法：
    python atvv_bench.py                                   # 最新一次采集，火山
    python atvv_bench.py --fixtures fixtures/xiaomi/<ts> --provider all --rounds 2
    python atvv_bench.py --gain-db 6 --no-smooth           # 调参闭环
    python atvv_bench.py --self-test                       # 离线自测（不联网）
    python atvv_bench.py --emit-demo-fixture DIR --pcm-source corpus/short_01.pcm \
        --text "今天天气不错。"                            # 合成 golden fixtures

参考答案：fixtures 目录下的 refs.json（{"session_1": "参考文本", ...}），
或 --refs 指定路径。无参考答案时退出码 2（不伪造 CER）。
退出码：0 运行完成；1 无结果/自测失败；2 凭据/fixtures/参考答案缺失。
"""
from __future__ import annotations

import argparse
import json
import math
import sys
import tempfile
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from atvv_capture import (ImaAdpcmDecoder, apply_gain, ima_encode,  # noqa: E402
                          read_wav_samples, smooth3, ts, write_wav)
from asr_bench import tencent, volcengine  # noqa: E402
from asr_bench.metrics import cer  # noqa: E402
from asr_bench.report import build_json_report, build_markdown  # noqa: E402
from asr_bench.result import ClipResult  # noqa: E402
from asr_bench.wsproto import load_windows_config  # noqa: E402

FIXTURES_ROOT = HERE / "fixtures" / "xiaomi"
DEFAULT_OUT = HERE / "bench_results"
# 桌面端 XiaomiAtvvSession::Options 默认 gain_db=12.0，bench 默认对齐它，
# 使评测口径 = 桌面端真实送 ASR 的音频（模 Opus 有损编码一步）。
DEFAULT_GAIN_DB = 12.0


# ---- fixtures 解析与会话解码 ------------------------------------------------
def resolve_fixtures_dir(arg: str | None) -> Path | None:
    """--fixtures 可指向采集目录本身或其父目录（取最新子目录）；缺省取
    scripts/e2e_test/fixtures/xiaomi/ 下最新一次采集。无可用目录返回 None。"""
    if arg:
        p = Path(arg)
        if list(p.glob("session_*.json")):
            return p
        children = sorted(c for c in p.glob("*") if c.is_dir()
                          and list(c.glob("session_*.json")))
        return children[-1] if children else None
    if not FIXTURES_ROOT.is_dir():
        return None
    children = sorted(c for c in FIXTURES_ROOT.iterdir() if c.is_dir()
                      and list(c.glob("session_*.json")))
    return children[-1] if children else None


def load_sessions(fixtures_dir: Path) -> list[dict]:
    """按 session 序号加载 sidecar + adpcm 路径。
    文件名序号非数字、sidecar JSON 畸形或缺键（files.adpcm / session）时
    WARN 跳过该条，不崩溃（采集目录可能混入手工产物）。"""
    indexed = []
    for sidecar_path in fixtures_dir.glob("session_*.json"):
        try:
            index = int(sidecar_path.stem.split("_")[1])
        except (ValueError, IndexError):
            print(f"WARN: {sidecar_path.name} 文件名序号非数字，跳过",
                  file=sys.stderr)
            continue
        indexed.append((index, sidecar_path))
    sessions = []
    for _index, sidecar_path in sorted(indexed):
        try:
            sidecar = json.loads(sidecar_path.read_text(encoding="utf-8"))
            adpcm_path = fixtures_dir / sidecar["files"]["adpcm"]
            session_no = sidecar["session"]
        except (KeyError, json.JSONDecodeError) as exc:
            print(f"WARN: {sidecar_path.name} 畸形或缺键（{exc}），跳过",
                  file=sys.stderr)
            continue
        if not adpcm_path.exists():
            print(f"WARN: {adpcm_path.name} 缺失，跳过 session {session_no}",
                  file=sys.stderr)
            continue
        sessions.append({"sidecar": sidecar, "adpcm_path": adpcm_path})
    return sessions


def decode_session_pcm(adpcm: bytes, sidecar: dict) -> list:
    """按 sidecar 段落（offset/bytes/predictor/step_index）复现采集时的解码路径。
    段落区间越界（offset+bytes 超出 ADPCM 总长）时 WARN 跳过该段——对齐
    C++ golden 测试 assert 的边界意图，但 bench 侧不崩溃。"""
    decoder = ImaAdpcmDecoder()
    pcm = []
    for i, seg in enumerate(sidecar["segments"]):
        start, nbytes = seg["offset"], seg["bytes"]
        if start < 0 or nbytes < 0 or start + nbytes > len(adpcm):
            print(f"WARN: 段落 {i} 越界（offset={start} bytes={nbytes}，"
                  f"ADPCM 总长 {len(adpcm)}），跳过该段", file=sys.stderr)
            continue
        decoder.reset(seg["predictor"], seg["step_index"])
        pcm += decoder.decode(adpcm[start:start + nbytes])
    return pcm


def load_refs(fixtures_dir: Path, refs_arg: str | None) -> dict[str, str] | None:
    """参考答案：--refs 指定路径优先，否则 fixtures 目录下 refs.json。
    文件格式 {"session_1": "参考文本", ...}。缺失返回 None。"""
    path = Path(refs_arg) if refs_arg else fixtures_dir / "refs.json"
    if not path.exists():
        return None
    refs = json.loads(path.read_text(encoding="utf-8"))
    return {k: str(v) for k, v in refs.items()}


def pcm_to_s16le(samples: list) -> bytes:
    import array
    pcm = array.array("h", samples)
    if sys.byteorder == "big":
        pcm.byteswap()
    return pcm.tobytes()


# ---- 评测主流程 -------------------------------------------------------------
def run_one_session(provider: str, session: dict, pcm_path: Path, reference: str,
                    round_no: int, cfg: dict, timeout: float,
                    fixtures_name: str) -> ClipResult:
    sidecar = session["sidecar"]
    clip_id = f"{fixtures_name}/session_{sidecar['session']}"
    duration_s = sidecar["samples"] / float(sidecar["sample_rate"])
    common = dict(duration_s=duration_s, clip_id=clip_id, round_no=round_no,
                  category="xiaomi_atvv", reference=reference,
                  audio_format="pcm")
    if provider == "volcengine":
        return volcengine.run_clip(pcm_path, api_key=cfg["volcengine_api_key"],
                                   timeout=timeout, **common)
    if provider == "tencent":
        return tencent.run_clip(pcm_path, secret_id=cfg["tencent_secret_id"],
                                secret_key=cfg["tencent_secret_key"],
                                appid=cfg["tencent_appid"], timeout=timeout, **common)
    raise ValueError(f"unknown provider {provider}")


def run_bench(args: argparse.Namespace) -> int:
    fixtures_dir = resolve_fixtures_dir(args.fixtures)
    if fixtures_dir is None:
        print(f"未找到 ATVV fixtures（{args.fixtures or FIXTURES_ROOT}）。"
              f"先用 atvv_capture.py 采集，或 --emit-demo-fixture 合成。",
              file=sys.stderr)
        return 2
    refs = load_refs(fixtures_dir, args.refs)
    if refs is None:
        print(f"缺少参考答案：{fixtures_dir}/refs.json 不存在（可用 --refs 指定）。"
              f"无参考答案不评 CER，退出。", file=sys.stderr)
        return 2

    sessions = load_sessions(fixtures_dir)
    if not sessions:
        print(f"{fixtures_dir} 下无 session_*.json", file=sys.stderr)
        return 2

    providers = (["volcengine", "tencent"] if args.provider == "all"
                 else [args.provider])
    cfg = load_windows_config()
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

    # 逐会话解码 + 后处理（只取有参考答案、16kHz 的会话）
    prepared = []
    for session in sessions:
        sidecar = session["sidecar"]
        key = f"session_{sidecar['session']}"
        if key not in refs:
            print(f"WARN: {key} 无参考答案，跳过", file=sys.stderr)
            continue
        if sidecar["sample_rate"] != 16000:
            print(f"WARN: {key} 采样率 {sidecar['sample_rate']} != 16000，跳过",
                  file=sys.stderr)
            continue
        adpcm = session["adpcm_path"].read_bytes()
        pcm = decode_session_pcm(adpcm, sidecar)
        if not args.no_smooth:
            pcm = smooth3(pcm)
        pcm = apply_gain(pcm, args.gain_db)
        prepared.append((session, pcm, refs[key]))
    if not prepared:
        print("无可用会话（检查 refs.json 与采样率）", file=sys.stderr)
        return 2

    print(f"{ts()} fixtures={fixtures_dir} sessions={len(prepared)} "
          f"gain_db={args.gain_db} smooth={not args.no_smooth} "
          f"providers={providers} rounds={args.rounds}", flush=True)

    results: list[ClipResult] = []
    total = len(providers) * args.rounds * len(prepared)
    done = 0
    with tempfile.TemporaryDirectory() as td:
        for provider in providers:
            for round_no in range(1, args.rounds + 1):
                for session, pcm, reference in prepared:
                    done += 1
                    pcm_path = Path(td) / f"session_{session['sidecar']['session']}.pcm"
                    pcm_path.write_bytes(pcm_to_s16le(pcm))
                    res = run_one_session(provider, session, pcm_path, reference,
                                          round_no, cfg, args.timeout,
                                          fixtures_dir.name)
                    results.append(res)
                    if res.success:
                        note = f" cer={cer(res.reference, res.final_text):.3f}"
                        if res.tail_latency_ms is not None:
                            note += f" tail={res.tail_latency_ms:.0f}ms"
                        note += f' text="{res.final_text}"'
                    else:
                        note = f" err={res.error}"
                    print(f"[{done}/{total}] {'OK ' if res.success else 'FAIL'} "
                          f"{provider} r{round_no} "
                          f"session_{session['sidecar']['session']}{note}",
                          flush=True)

    report = build_json_report(results, corpus_size=len(prepared),
                               rounds=args.rounds)
    report["fixtures"] = str(fixtures_dir)
    report["audio_format"] = "pcm"
    report["gain_db"] = args.gain_db
    report["smooth"] = not args.no_smooth
    # synthetic 标记传播：任一会话 sidecar 标了 synthetic 即整份报告标记
    # （合成 fixtures 只验证链路，不作识别率结论）
    synthetic = any(s["sidecar"].get("synthetic") for s, _, _ in prepared)
    if synthetic:
        report["synthetic"] = True
        print("⚠️ 输入为合成 fixtures（synthetic=true），本报告不作识别率结论",
              flush=True)
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = args.stamp
    json_path = out_dir / f"atvv_bench_{stamp}.json"
    json_path.write_text(json.dumps(report, ensure_ascii=False, indent=2),
                         encoding="utf-8")
    md_path = out_dir / f"atvv_bench_{stamp}.md"
    md_path.write_text(build_markdown(report), encoding="utf-8")
    print(f"\nJSON: {json_path}")
    print(f"MD:   {md_path}")
    for p, s in report["providers"].items():
        print(f"{p}: success={s['success']}/{s['runs']} cer_mean={s['cer_mean']} "
              f"cer_median={s['cer_median']}")
    return 0


# ---- 合成 demo fixtures（golden 对拍 / 管道冒烟用） --------------------------
def emit_demo_fixture(args: argparse.Namespace) -> int:
    """把一段 PCM（--pcm-source 的 s16le 16k 文件，缺省合成正弦扫频）编码为
    ADPCM 并写出完整 capture 格式目录。产物标记 synthetic: true——它不是真机
    数据，仅用于 golden 对拍与管道冒烟，不得作为识别率结论。"""
    out_dir = Path(args.emit_demo_fixture)
    if (out_dir / "session_1.adpcm").exists():
        print(f"{out_dir} 已存在 session_1.adpcm，拒绝覆盖", file=sys.stderr)
        return 2
    out_dir.mkdir(parents=True, exist_ok=True)

    source_desc = "sine"
    if args.pcm_source:
        raw = Path(args.pcm_source).read_bytes()
        if len(raw) % 2:
            raw = raw[:-1]
        import array
        arr = array.array("h")
        arr.frombytes(raw)
        if sys.byteorder == "big":
            arr.byteswap()
        pcm = list(arr)
        source_desc = str(args.pcm_source)
    else:
        # 合成 2s：起止淡出的 300Hz→1200Hz 扫频正弦（仅验证链路，非语音）
        n = 32000
        pcm = []
        for i in range(n):
            t = i / 16000.0
            env = min(1.0, i / 1600.0, (n - i) / 1600.0)
            phase = 2.0 * math.pi * (300.0 * t + 450.0 * t * t / (n / 16000.0))
            pcm.append(int(math.sin(phase) * 9000.0 * env))
        pcm = [max(-32768, min(32767, s)) for s in pcm]
    if len(pcm) % 2:
        pcm = pcm[:-1]  # ADPCM 4bit 打包需偶数样本

    encoded, _expected = ima_encode(pcm)
    # 自校验：解码回 predictor 轨迹
    decoder = ImaAdpcmDecoder()
    decoded = decoder.decode(encoded)
    assert decoded == _expected and len(decoded) == len(pcm)

    write_wav(out_dir / "session_1.raw.wav", decoded, 16000)
    write_wav(out_dir / "session_1.wav",
              apply_gain(smooth3(decoded), args.gain_db), 16000)
    (out_dir / "session_1.adpcm").write_bytes(encoded)
    sidecar = {
        "session": 1,
        "codec": 0x02,
        "sample_rate": 16000,
        "frame_len": 120,
        "gain_db": args.gain_db,
        "adpcm_bytes": len(encoded),
        "consumed_adpcm_bytes": len(encoded),
        "samples": len(decoded),
        "duration_s": round(len(decoded) / 16000.0, 3),
        "first_audio_latency_ms": None,
        "segments": [{"offset": 0, "bytes": len(encoded),
                      "predictor": 0, "step_index": 0}],
        "files": {"adpcm": "session_1.adpcm", "wav": "session_1.wav",
                  "raw_wav": "session_1.raw.wav"},
        "synthetic": True,
        "source": source_desc,
    }
    (out_dir / "session_1.json").write_text(
        json.dumps(sidecar, ensure_ascii=False, indent=2), encoding="utf-8")
    events = [
        {"t": 0.0, "event": "capture_config", "synthetic": True,
         "source": source_desc, "gain_db": args.gain_db},
        {"t": 0.0, "event": "caps", "version": "0x0100", "codecs": 2,
         "interaction": 3, "frame_len": 120, "legacy": False},
        {"t": 0.0, "event": "stream_start", "interaction": 3, "codec": 2,
         "session_id": 1},
        {"t": 0.0, "event": "stop"},
        {"t": 0.0, "event": "session_saved", "session": 1,
         "frames": len(encoded) // 120, "adpcm_bytes": len(encoded),
         "samples": len(decoded), "duration_s": sidecar["duration_s"],
         "sample_rate": 16000, "first_audio_latency_ms": None,
         "file_adpcm": "session_1.adpcm", "file_wav": "session_1.wav"},
    ]
    with open(out_dir / "events.jsonl", "w", encoding="utf-8") as fp:
        for ev in events:
            fp.write(json.dumps(ev, ensure_ascii=False) + "\n")
    if args.text:
        (out_dir / "refs.json").write_text(
            json.dumps({"session_1": args.text}, ensure_ascii=False, indent=2),
            encoding="utf-8")
    print(f"demo fixture written: {out_dir} "
          f"({len(encoded)} adpcm bytes, {len(decoded)} samples, "
          f"{sidecar['duration_s']}s, source={source_desc})")
    return 0


# ---- 自测（离线，不联网不连设备） ---------------------------------------------
def self_test() -> int:
    failures = []

    def check(name: str, fn) -> None:
        try:
            fn()
            print(f"PASS {name}", flush=True)
        except AssertionError as exc:
            failures.append(name)
            print(f"FAIL {name}: {exc}", flush=True)

    def test_demo_fixture_roundtrip() -> None:
        import types
        with tempfile.TemporaryDirectory() as td:
            out = Path(td) / "demo"
            args = types.SimpleNamespace(emit_demo_fixture=str(out),
                                         pcm_source=None, text="测试文本。",
                                         gain_db=6.0)
            assert emit_demo_fixture(args) == 0
            sessions = load_sessions(out)
            assert len(sessions) == 1
            sidecar = sessions[0]["sidecar"]
            assert sidecar["synthetic"] and sidecar["sample_rate"] == 16000
            adpcm = sessions[0]["adpcm_path"].read_bytes()
            pcm = decode_session_pcm(adpcm, sidecar)
            # 与 raw.wav 逐样本一致
            assert pcm == read_wav_samples(out / "session_1.raw.wav")
            # wav = raw 经 smooth+gain(6dB)
            assert (apply_gain(smooth3(pcm), 6.0)
                    == read_wav_samples(out / "session_1.wav"))
            # refs.json 已写
            refs = load_refs(out, None)
            assert refs == {"session_1": "测试文本。"}
            # 重复生成拒绝覆盖
            assert emit_demo_fixture(args) == 2

    def test_pcm_source_fixture() -> None:
        import types
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / "src.pcm"
            src.write_bytes(pcm_to_s16le([0, 1000, -1000, 3000] * 500))
            out = Path(td) / "demo2"
            args = types.SimpleNamespace(emit_demo_fixture=str(out),
                                         pcm_source=str(src), text=None,
                                         gain_db=0.0)
            assert emit_demo_fixture(args) == 0
            sidecar = json.loads((out / "session_1.json").read_text("utf-8"))
            assert sidecar["source"] == str(src) and sidecar["samples"] == 2000
            assert not (out / "refs.json").exists()  # 无 --text 不写 refs

    def test_fixtures_resolution() -> None:
        import types  # noqa: F401
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            assert resolve_fixtures_dir(str(root)) is None
            for name in ("20260101_000000", "20260102_000000"):
                d = root / name
                d.mkdir()
                (d / "session_1.json").write_text("{}", encoding="utf-8")
            # 父目录 -> 取最新子目录
            assert resolve_fixtures_dir(str(root)).name == "20260102_000000"
            # 直接指采集目录
            assert resolve_fixtures_dir(str(root / "20260101_000000")).name \
                == "20260101_000000"

    def test_pcm_payload_format() -> None:
        payload = json.loads(volcengine.session_payload(
            "rid", audio_format="pcm").decode("utf-8"))
        assert payload["audio"]["format"] == "pcm"
        assert "codec" not in payload["audio"]
        assert payload["audio"]["rate"] == 16000
        # 默认 ogg 路径不受影响
        payload = json.loads(volcengine.session_payload("rid").decode("utf-8"))
        assert payload["audio"] == {"format": "ogg", "codec": "opus",
                                    "rate": 16000, "bits": 16, "channel": 1}
        url = tencent.build_signed_url("sid", "skey", "12345", "vid",
                                       voice_format="1")
        assert "voice_format=1" in url
        # 腾讯 pcm 切片逻辑：3200 字节一档
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "a.pcm"
            p.write_bytes(b"\x00" * 7000)
            # 直接验证切片参数（不发网络）：7000 -> 3 片
            raw = p.read_bytes()
            frames = [raw[i:i + 3200] for i in range(0, len(raw), 3200)]
            assert [len(f) for f in frames] == [3200, 3200, 600]

    def test_decode_session_segments() -> None:
        # 多段（含 SYNC 丢弃字节）的复现解码
        pcm = [int(math.sin(i * 0.1) * 8000) for i in range(800)]
        encoded, _ = ima_encode(pcm)
        sidecar = {"segments": [
            {"offset": 0, "bytes": 120, "predictor": 0, "step_index": 0},
            {"offset": 200, "bytes": 200, "predictor": 0, "step_index": 0},
        ]}
        dec = ImaAdpcmDecoder()
        seg0 = dec.decode(encoded[:120])
        dec.reset(0, 0)
        seg1 = dec.decode(encoded[200:400])
        assert decode_session_pcm(encoded, sidecar) == seg0 + seg1

    def test_report_markdown_annotations() -> None:
        report = build_json_report([], corpus_size=1, rounds=1)
        # 缺 audio_format 字段：保持既有 ogg 文案，无警告行
        md = build_markdown(report)
        assert "本地 Ogg Opus 语料" in md and "⚠️" not in md
        # pcm + synthetic：文案与警告行都出现
        report["audio_format"] = "pcm"
        report["synthetic"] = True
        md = build_markdown(report)
        assert "本地 16kHz PCM 语料" in md
        assert "⚠️ 数据为合成 fixtures，不作识别率结论" in md

    def test_load_sessions_robustness() -> None:
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            (d / "s1.adpcm").write_bytes(b"\x00" * 8)
            (d / "session_1.json").write_text(json.dumps(
                {"session": 1, "files": {"adpcm": "s1.adpcm"}}),
                encoding="utf-8")
            # 文件名序号非数字 / sidecar 缺 files 键 / adpcm 缺失：均 WARN 跳过
            (d / "session_x.json").write_text("{}", encoding="utf-8")
            (d / "session_2.json").write_text("{}", encoding="utf-8")
            (d / "session_3.json").write_text(json.dumps(
                {"session": 3, "files": {"adpcm": "missing.adpcm"}}),
                encoding="utf-8")
            sessions = load_sessions(d)
            assert len(sessions) == 1 and sessions[0]["sidecar"]["session"] == 1

    def test_decode_session_out_of_bounds() -> None:
        pcm = [int(math.sin(i * 0.1) * 8000) for i in range(400)]
        encoded, _ = ima_encode(pcm)
        good = ImaAdpcmDecoder().decode(encoded[:120])
        sidecar = {"segments": [
            {"offset": 0, "bytes": 120, "predictor": 0, "step_index": 0},
            # 越界段：offset+bytes 超出总长 → WARN 跳过，不崩溃
            {"offset": len(encoded) - 10, "bytes": 100,
             "predictor": 0, "step_index": 0},
        ]}
        assert decode_session_pcm(encoded, sidecar) == good

    check("demo_fixture_roundtrip", test_demo_fixture_roundtrip)
    check("pcm_source_fixture", test_pcm_source_fixture)
    check("fixtures_resolution", test_fixtures_resolution)
    check("pcm_payload_format", test_pcm_payload_format)
    check("decode_session_segments", test_decode_session_segments)
    check("report_markdown_annotations", test_report_markdown_annotations)
    check("load_sessions_robustness", test_load_sessions_robustness)
    check("decode_session_out_of_bounds", test_decode_session_out_of_bounds)

    total = 8
    print(f"---- self-test: {total - len(failures)}/{total} passed ----",
          flush=True)
    return 1 if failures else 0


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except Exception:  # noqa: BLE001
        pass
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--fixtures", default=None,
                    help="采集目录或其父目录（默认取 "
                         "scripts/e2e_test/fixtures/xiaomi/ 下最新一次采集）")
    ap.add_argument("--refs", default=None,
                    help="参考答案 JSON（默认 <fixtures>/refs.json，"
                         "格式 {\"session_1\": \"文本\"}）")
    ap.add_argument("--provider", choices=["all", "volcengine", "tencent"],
                    default="volcengine")
    ap.add_argument("--rounds", type=int, default=1, help="回放轮数（默认 1）")
    ap.add_argument("--timeout", type=float, default=30.0, help="单条超时秒数")
    ap.add_argument("--gain-db", type=float, default=DEFAULT_GAIN_DB,
                    help=f"后处理增益 dB（默认 {DEFAULT_GAIN_DB}，对齐桌面端）")
    ap.add_argument("--no-smooth", action="store_true", help="关闭三点平滑（消融）")
    ap.add_argument("--out", default=str(DEFAULT_OUT), help="报告输出目录")
    ap.add_argument("--stamp", default=datetime.now().strftime("%Y%m%d-%H%M%S"))
    ap.add_argument("--self-test", action="store_true",
                    help="离线自测（解码/侧车/格式/目录解析），不联网")
    ap.add_argument("--emit-demo-fixture", metavar="DIR", default=None,
                    help="合成 capture 格式 demo fixtures（synthetic 标记）")
    ap.add_argument("--pcm-source", default=None,
                    help="demo fixture 的 PCM 源（s16le 16k mono 文件，"
                         "缺省合成正弦扫频）")
    ap.add_argument("--text", default=None,
                    help="demo fixture 的参考文本（写入 refs.json）")
    ap.add_argument("--text-file", default=None,
                    help="参考文本从文件读（UTF-8；中文场景避免命令行编码问题）")
    args = ap.parse_args()

    if args.text is None and args.text_file:
        args.text = Path(args.text_file).read_text(encoding="utf-8").strip()

    if args.self_test:
        return self_test()
    if args.emit_demo_fixture:
        return emit_demo_fixture(args)
    return run_bench(args)


if __name__ == "__main__":
    sys.exit(main())
