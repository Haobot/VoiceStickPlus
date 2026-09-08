#!/usr/bin/env python3
"""M0 评测平台后端（FastAPI，仅监听 127.0.0.1）。

功能（用户需求②③④）：
- 引擎配置界面 API：查看/切换 active 引擎（写回 m0/config.toml）
- 单句试听与识别：测试集条目或上传 wav，走 active 引擎端到端识别
- 闭环评测：后台子进程跑 sim_compare.py，轮询进度与结果

安全红线：凭据只在服务端解析，界面只展示脱敏摘要；本服务不监听外网。

用法（m0/ 目录下）:
    .venv/Scripts/python.exe app.py            # http://127.0.0.1:8765
"""
from __future__ import annotations

import atexit
import json
import re
import subprocess
import sys
import threading
import time
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "src"))

from fastapi import FastAPI, File, Form, HTTPException, UploadFile
from fastapi.responses import FileResponse, JSONResponse

from m0_asr.audio_utils import read_wave, resample
from m0_asr.platform_config import (
    load_platform_config,
    resolve_tencent_credentials,
)
from m0_asr.providers import get_provider, registered_provider_names

M0_ROOT = Path(__file__).resolve().parent
CONFIG_PATH = M0_ROOT / "config.toml"
WEB_DIR = M0_ROOT / "web"
TESTSET_JSON = M0_ROOT / "data" / "texts" / "testset.json"
WAVS_DIR = M0_ROOT / "data" / "wavs"
LATEST_JSON = M0_ROOT / "data" / "results" / "sim_compare_latest.json"
REPORT_MD = M0_ROOT / "docs" / "cloud_vs_local_report.md"

app = FastAPI(title="VoiceStick M0 评测平台")

# 引擎实例缓存（模型加载昂贵，进程内单例；凭据启动时解析一次）
_CREDENTIALS = None
_PROVIDERS: dict[str, object] = {}
_PROVIDER_LOCK = threading.Lock()

# 评测子进程状态（单实例串行，避免并发烧云端配额）
_EVAL_STATE: dict = {"running": False, "started_at": None, "lines": [],
                     "returncode": None, "process": None}
_EVAL_LOCK = threading.Lock()


def _credentials():
    global _CREDENTIALS
    if _CREDENTIALS is None:
        _CREDENTIALS = resolve_tencent_credentials(m0_config=CONFIG_PATH)
    return _CREDENTIALS


def _provider(name: str):
    """获取/缓存引擎实例。"""
    with _PROVIDER_LOCK:
        if name not in _PROVIDERS:
            _PROVIDERS[name] = get_provider(name, credentials=_credentials())
        return _PROVIDERS[name]


# ---------- 引擎配置 ----------

@app.get("/api/engines")
def list_engines():
    config = load_platform_config(CONFIG_PATH)
    engines = []
    for name in registered_provider_names():
        provider = _provider(name)
        status = provider.health_check()
        extra = {}
        if provider.kind == "cloud":
            cred = _credentials()
            extra["credentials"] = cred.masked_summary() if cred else "未解析到凭据"
        engines.append({"name": name, "kind": provider.kind,
                        "health_ok": status.ok, "detail": status.detail, **extra})
    return {"active": config.active_engine, "engines": engines}


@app.post("/api/engine/active")
def set_active_engine(body: dict):
    name = str(body.get("name", ""))
    if name not in registered_provider_names():
        raise HTTPException(400, f"未知引擎: {name}")
    _save_active_engine(CONFIG_PATH, name)
    # 引擎切换后清缓存无必要（实例可复用），直接返回
    return {"active": name}


def _save_active_engine(path: Path, engine_name: str) -> None:
    """把 [engine] active 写回 m0/config.toml，保留其余内容（可能含凭据）。"""
    text = path.read_text(encoding="utf-8") if path.exists() else ""
    active_line = f'active = "{engine_name}"'
    if re.search(r"(?m)^active\s*=\s*\"[^\"]*\"", text):
        text = re.sub(r"(?m)^active\s*=\s*\"[^\"]*\"", active_line, text)
    elif re.search(r"(?m)^\[engine\]", text):
        text = re.sub(r"(?m)^\[engine\]", f"[engine]\n{active_line}", text)
    else:
        text = text.rstrip() + f"\n\n[engine]\n{active_line}\n"
    path.write_text(text, encoding="utf-8")


# ---------- 测试集与试听 ----------

@app.get("/api/audio/list")
def audio_list():
    data = json.loads(TESTSET_JSON.read_text(encoding="utf-8"))
    entries = []
    for section, items in data.items():
        for item in items:
            wav = WAVS_DIR / f"{item['id']}.wav"
            entries.append({"id": item["id"], "group": section,
                            "hotword": item.get("hotword"),
                            "text": item["text"], "has_wav": wav.exists()})
    return {"entries": entries}


@app.get("/api/audio/file/{entry_id}")
def audio_file(entry_id: str):
    wav = WAVS_DIR / f"{entry_id}.wav"
    if not (wav.exists() and entry_id.replace("_", "").isalnum()):
        raise HTTPException(404, "音频不存在")
    return FileResponse(wav, media_type="audio/wav")


@app.post("/api/transcribe")
async def transcribe(entry_id: str = Form(None), file: UploadFile = File(None)):
    """单句识别：entry_id 走测试集，file 走上传；引擎取当前 active。"""
    active = load_platform_config(CONFIG_PATH).active_engine
    provider = _provider(active)
    status = provider.health_check()
    if not status.ok:
        raise HTTPException(400, f"引擎不可用: {status.detail}")

    if entry_id:
        wav = WAVS_DIR / f"{entry_id}.wav"
        if not wav.exists():
            raise HTTPException(404, "音频不存在")
        outcome = provider.transcribe_file(wav)
    elif file is not None:
        tmp = M0_ROOT / "data" / "wavs" / f"upload_{uuid.uuid4().hex[:8]}.wav"
        tmp.write_bytes(await file.read())
        try:
            samples, rate = read_wave(tmp)
            if rate != 16000:  # 上传音频归一到 16k 再识别
                import soundfile as sf
                resampled = resample(samples, rate, 16000)
                sf.write(tmp, resampled, 16000)
            outcome = provider.transcribe_file(tmp)
        finally:
            tmp.unlink(missing_ok=True)
    else:
        raise HTTPException(400, "entry_id 与 file 必须二选一")
    return {"engine": active, "text": outcome.text,
            "elapsed_seconds": round(outcome.elapsed_seconds, 3),
            "audio_seconds": round(outcome.audio_seconds, 2),
            "rtf": round(outcome.rtf, 4) if outcome.audio_seconds else None,
            "error": outcome.error}


# ---------- 闭环评测 ----------

@app.post("/api/eval/run")
def eval_run():
    with _EVAL_LOCK:
        if _EVAL_STATE["running"]:
            raise HTTPException(409, "已有评测在运行")
        _EVAL_STATE.update(running=True, started_at=time.time(), lines=[],
                           returncode=None)
        proc = subprocess.Popen(
            [sys.executable, "sim_compare.py"],
            cwd=M0_ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, encoding="utf-8", errors="replace",
        )
        _EVAL_STATE["process"] = proc
    threading.Thread(target=_drain_eval_output, args=(proc,), daemon=True).start()
    return {"started": True}


def _drain_eval_output(proc: subprocess.Popen) -> None:
    for line in proc.stdout:
        _EVAL_STATE["lines"].append(line.rstrip())
    _EVAL_STATE["returncode"] = proc.wait()
    _EVAL_STATE["running"] = False


@app.get("/api/eval/status")
def eval_status():
    with _EVAL_LOCK:
        done = sum(1 for l in _EVAL_STATE["lines"] if l.startswith("  ["))
        return {"running": _EVAL_STATE["running"],
                "recognized_sentences": done,
                "returncode": _EVAL_STATE["returncode"],
                "log_tail": _EVAL_STATE["lines"][-8:]}


@app.get("/api/eval/report")
def eval_report():
    if not LATEST_JSON.exists():
        return JSONResponse({"available": False})
    payload = json.loads(LATEST_JSON.read_text(encoding="utf-8"))
    return {"available": True, "timestamp": payload["timestamp"],
            "metrics": payload["metrics"],
            "report_md": REPORT_MD.read_text(encoding="utf-8")
            if REPORT_MD.exists() else ""}


@app.get("/")
def index():
    return FileResponse(WEB_DIR / "index.html")


def _kill_eval_process() -> None:
    proc = _EVAL_STATE.get("process")
    if proc is not None and proc.poll() is None:
        proc.kill()


atexit.register(_kill_eval_process)


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host="127.0.0.1", port=8765, log_level="info")
