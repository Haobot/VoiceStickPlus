"""腾讯实时 ASR 评测回放（asr/v2 WebSocket，opus 封装）。

签名 URL 与帧封装复刻 replay_tencent_asr.py /
desktop/windows/src/asr_client_tencent.cc（ExtractTencentOpusFrame 小端长度）。
凭据从 config.toml 读取（tencent_secret_id / tencent_secret_key / tencent_appid），
与桌面端默认一致使用 engine_model_type=16k_zh_en、voice_format=10(opus)、needvad=1。
"""
from __future__ import annotations

import base64
import hashlib
import hmac
import json
import random
import select
import socket
import ssl
import time
import urllib.parse
import uuid
from pathlib import Path

from .result import ClipResult
from .wsproto import demux_ogg_packets, recv_ws_frame, send_ws_frame, websocket_handshake

TENCENT_HOST = "asr.cloud.tencent.com"
DEFAULT_ENGINE = "16k_zh_en"


def build_signed_url(secret_id: str, secret_key: str, appid: str,
                     voice_id: str, engine: str = DEFAULT_ENGINE,
                     needvad: str = "1", voice_format: str = "10",
                     hotword_id: str = "", hotword_list: str = "") -> str:
    """hotword_id=控制台热词表 ID；hotword_list=临时热词表（"词|权重" 逗号分隔，
    最多 128 词）。两者同传时服务端只有 hotword_list 生效。
    参数值与桌面端 asr_client_tencent.cc 一致原样进签名原文与 URL，
    不做 URL encode（中文/竖线原样进 query，实测服务端接受）。"""
    params = {
        "secretid": secret_id,
        "timestamp": str(int(time.time())),
        "expired": str(int(time.time()) + 86400),
        "nonce": str(random.randint(1, 2**31 - 1)),
        "engine_model_type": engine,
        "voice_format": voice_format,
        "needvad": needvad,
        "voice_id": voice_id,
    }
    if hotword_id:
        params["hotword_id"] = hotword_id
    if hotword_list:
        params["hotword_list"] = hotword_list
    query = "&".join(f"{k}={params[k]}" for k in sorted(params))
    sign_str = f"{TENCENT_HOST}/asr/v2/{appid}?{query}"
    sig = base64.b64encode(
        hmac.new(secret_key.encode(), sign_str.encode(), hashlib.sha1).digest()
    ).decode()
    # 签名原文用原值；最终 URL 逐值 percent-encode（hotword_list 含中文/竖线时
    # 必须编码，否则 HTTP 请求行无法以 ASCII 发送）。unreserved 字符不受影响。
    encoded_query = "&".join(f"{k}={urllib.parse.quote(params[k], safe='')}"
                             for k in sorted(params))
    return (f"wss://{TENCENT_HOST}/asr/v2/{appid}?{encoded_query}"
            f"&signature={urllib.parse.quote(sig, safe='')}")


def wrap_frame(packet: bytes) -> bytes:
    """"opus" + 长度（2 字节小端，与桌面端 ExtractTencentOpusFrame 一致）+ Opus 帧。"""
    return b"opus" + bytes([len(packet) & 0xFF, (len(packet) >> 8) & 0xFF]) + packet


def run_clip(ogg_path: Path, *, secret_id: str, secret_key: str, appid: str,
             engine: str = DEFAULT_ENGINE, timeout: float = 20.0,
             duration_s: float = 0.0, clip_id: str = "", round_no: int = 0,
             category: str = "", reference: str = "",
             hotword_id: str = "", hotword_list: str = "") -> ClipResult:
    """回放单条 ogg 到腾讯 ASR，返回结构化结果。异常不外抛，记入 error。"""
    res = ClipResult(clip_id=clip_id, provider="tencent", round=round_no,
                     category=category, reference=reference, duration_s=duration_s)
    frames = demux_ogg_packets(ogg_path)
    if not frames:
        res.error = "no opus frames demuxed"
        return res

    sock = None
    try:
        url = build_signed_url(secret_id, secret_key, appid, str(uuid.uuid4()), engine,
                               hotword_id=hotword_id, hotword_list=hotword_list)
        sock = websocket_handshake(url, timeout)
        sock.settimeout(timeout)

        n_frames = len(frames)
        pace_s = (duration_s / n_frames) if duration_s > 0 else 0.04

        final_text: list[str] = []
        got_final = False

        t_start = time.monotonic()
        t_audio_end = t_start
        for pkt in frames:
            send_ws_frame(sock, 0x2, wrap_frame(pkt))
            t_audio_end = time.monotonic()
            # 只收已到达的结果：select 零超时轮询，不做带超时的阻塞 recv。
            # （旧实现每帧 settimeout(1ms) 后 recv，Windows 定时器粒度使每帧
            # 多花约 10-15ms，长音频总发送时间被拉长 50% 以上。）
            _drain_available(sock, res, final_text, t_start)
            time.sleep(pace_s)

        send_ws_frame(sock, 0x1, '{"type":"end"}')

        deadline = time.monotonic() + timeout
        sock.settimeout(5)
        while time.monotonic() < deadline and not got_final:
            try:
                opcode, payload = recv_ws_frame(sock)
            except Exception:  # noqa: BLE001 - 超时或连接关闭
                break
            now = time.monotonic()
            got_final = _handle_message(res, opcode, payload, final_text, t_start)
            if got_final:
                res.tail_latency_ms = (now - t_audio_end) * 1000.0
                res.total_latency_ms = (now - t_start) * 1000.0

        res.final_text = "".join(final_text)
        if not got_final:
            res.error = "timeout: no final=1"
        elif not res.final_text:
            res.error = "empty transcript"
        res.success = not res.error and not res.server_errors
    except Exception as e:  # noqa: BLE001 - 连接/协议异常记为失败用例
        res.error = f"{type(e).__name__}: {e}"
    finally:
        if sock is not None:
            try:
                sock.close()
            except OSError:
                pass
    return res


def _sock_readable(sock: socket.socket) -> bool:
    """零超时检查是否有数据可读（含 SSL 层已解密缓冲）。"""
    try:
        if isinstance(sock, ssl.SSLSocket) and sock.pending() > 0:
            return True
        r, _, _ = select.select([sock], [], [], 0)
        return bool(r)
    except (OSError, ValueError):
        return False


def _drain_available(sock: socket.socket, res: ClipResult,
                     final_text: list[str], t_start: float) -> None:
    """收干已到达的服务端消息；无数据时立即返回，不阻塞等待新数据。"""
    while _sock_readable(sock):
        # 帧已开始到达，给有限时间收完整帧（正常整帧随 TCP 一起到，不会等满）
        sock.settimeout(2.0)
        try:
            opcode, payload = recv_ws_frame(sock)
        except Exception:  # noqa: BLE001 - 半帧超时/连接异常，停止收
            return
        _handle_message(res, opcode, payload, final_text, t_start)


def _handle_message(res: ClipResult, opcode: int, payload: bytes,
                    final_text: list[str], t_start: float) -> bool:
    """处理一帧服务端消息，返回是否收到 final=1。"""
    if opcode == 0x9:  # ping 不处理（腾讯不回 ping）
        return False
    if opcode not in {0x1, 0x2}:
        return False
    try:
        obj = json.loads(payload.decode("utf-8", "replace"))
    except (json.JSONDecodeError, UnicodeDecodeError):
        return False
    if obj.get("code") != 0:
        res.server_errors.append(f"code={obj.get('code')} msg={obj.get('message', '')}")
        return False
    now = time.monotonic()
    r = obj.get("result")
    if r:
        res.partial_count += 1
        if res.first_partial_latency_ms is None:
            res.first_partial_latency_ms = (now - t_start) * 1000.0
        txt = r.get("voice_text_str", "")
        if r.get("slice_type") == 2 and txt:
            final_text.append(txt)
    return obj.get("final") == 1
