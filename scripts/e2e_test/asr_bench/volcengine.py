"""火山引擎 ASR 评测回放（bigmodel_async 可复用连接协议）。

协议帧与 replay_volcengine_asr.py / desktop/windows/src/asr_protocol.cc 一致。
评测默认不带热词/自学习表（客观基线）；音频按真实时长实时节奏分包发送。
"""
from __future__ import annotations

import json
import secrets
import socket
import struct
import time
from pathlib import Path

from .result import ClipResult
from .wsproto import recv_ws_frame, send_ws_frame, websocket_handshake

VOLCENGINE_URL = "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async"
DEFAULT_RESOURCE_ID = "volc.bigasr.sauc.duration"

EVENT_START_CONNECTION = 1
EVENT_FINISH_CONNECTION = 2
EVENT_START_SESSION = 100
EVENT_FINISH_SESSION = 102
EVENT_TASK_REQUEST = 200


def session_payload(resource_id: str, *, result_type: str = "full",
                    enable_nonstream: bool = True, enable_ddc: bool = True,
                    hotwords: list[str] | None = None,
                    boosting_table_id: str = "", correct_table_id: str = "") -> bytes:
    request = {
        "model_name": "bigmodel",
        "enable_nonstream": enable_nonstream,
        "show_utterances": False,
        "result_type": result_type,
        "enable_ddc": enable_ddc,
        "resource_id": resource_id,
    }
    corpus: dict[str, str] = {}
    if boosting_table_id:
        corpus["boosting_table_id"] = boosting_table_id
    if correct_table_id:
        corpus["correct_table_id"] = correct_table_id
    if hotwords:
        corpus["context"] = json.dumps({"hotwords": [{"word": w} for w in hotwords]},
                                       ensure_ascii=False, separators=(",", ":"))
    if corpus:
        request["corpus"] = corpus
    payload = {
        "user": {"uid": "voice-stick-asr-bench"},
        "audio": {"format": "ogg", "codec": "opus", "rate": 16000, "bits": 16, "channel": 1},
        "request": request,
    }
    return json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def connection_payload(resource_id: str, *, result_type: str = "full",
                       enable_nonstream: bool = True, enable_ddc: bool = True,
                       hotwords: list[str] | None = None,
                       boosting_table_id: str = "", correct_table_id: str = "") -> bytes:
    inner = session_payload(resource_id, result_type=result_type,
                            enable_nonstream=enable_nonstream, enable_ddc=enable_ddc,
                            hotwords=hotwords, boosting_table_id=boosting_table_id,
                            correct_table_id=correct_table_id)
    return b'{"namespace":"BidirectionalASR","event":0,"req_params":' + inner + b"}"


def make_event_frame(message_type: int, event: int, session_id: str,
                     serialization: int, payload: bytes) -> bytes:
    out = bytearray([0x11, (message_type << 4) | 0x04, serialization << 4, 0x00])
    out += struct.pack(">I", event)
    if session_id:
        session_bytes = session_id.encode("utf-8")
        out += struct.pack(">I", len(session_bytes)) + session_bytes
    out += struct.pack(">I", len(payload)) + payload
    return bytes(out)


def parse_event_frame(payload: bytes) -> tuple[int, str] | None:
    """解析服务端事件帧，返回 (event_id, payload_text)；错误帧返回 (-1, 描述)。"""
    if len(payload) < 4:
        return None
    message_type = payload[1] >> 4
    flags = payload[1] & 0x0F
    compression = payload[2] & 0x0F
    if message_type not in {0x09, 0x0B} or flags != 0x04 or compression != 0x00:
        if message_type == 0x0F:
            offset = (payload[0] & 0x0F) * 4
            if len(payload) >= offset + 8:
                code = struct.unpack(">I", payload[offset:offset + 4])[0]
                msg_size = struct.unpack(">I", payload[offset + 4:offset + 8])[0]
                msg = payload[offset + 8:offset + 8 + msg_size].decode("utf-8", "replace")
                return (-1, f"error {code}: {msg}")
        return None
    offset = (payload[0] & 0x0F) * 4
    if len(payload) < offset + 4:
        return None
    event_id = struct.unpack(">I", payload[offset:offset + 4])[0]
    offset += 4
    if len(payload) >= offset + 4:
        sid_size = struct.unpack(">I", payload[offset:offset + 4])[0]
        offset += 4 + sid_size
    if len(payload) < offset + 4:
        return (event_id, "")
    payload_size = struct.unpack(">I", payload[offset:offset + 4])[0]
    offset += 4
    text = payload[offset:offset + payload_size].decode("utf-8", "replace")
    return (event_id, text)


def run_clip(ogg_path: Path, *, api_key: str, resource_id: str = DEFAULT_RESOURCE_ID,
             url: str = VOLCENGINE_URL, timeout: float = 30.0,
             duration_s: float = 0.0, clip_id: str = "", round_no: int = 0,
             category: str = "", reference: str = "",
             result_type: str = "full", enable_nonstream: bool = True,
             enable_ddc: bool = True, hotwords: list[str] | None = None,
             boosting_table_id: str = "", correct_table_id: str = "") -> ClipResult:
    """回放单条 ogg 到火山 ASR，返回结构化结果。异常不外抛，记入 error。

    result_type / enable_nonstream / enable_ddc 默认与桌面端一致，
    消融实验（run_volc_ablation.py）通过覆盖这三个参数对比配置。
    hotwords=corpus.context 直传热词；boosting_table_id / correct_table_id
    为自学习平台热词表/替换词表 ID。
    """
    res = ClipResult(clip_id=clip_id, provider="volcengine", round=round_no,
                     category=category, reference=reference, duration_s=duration_s)
    ogg = ogg_path.read_bytes()
    if not ogg:
        res.error = "empty ogg"
        return res

    sock = None
    try:
        sock = websocket_handshake(url, timeout, extra_headers=[
            f"X-Api-Key: {api_key}",
            f"X-Api-Resource-Id: {resource_id}",
            f"X-Api-Request-Id: voice-stick-bench-{secrets.token_hex(8)}",
            "X-Api-Sequence: -1",
        ])
        session_id = secrets.token_hex(8)
        finals: list[str] = []
        partials: list[str] = []

        payload_kw = dict(result_type=result_type, enable_nonstream=enable_nonstream,
                          enable_ddc=enable_ddc, hotwords=hotwords,
                          boosting_table_id=boosting_table_id,
                          correct_table_id=correct_table_id)
        send_ws_frame(sock, 0x2, make_event_frame(0x01, EVENT_START_CONNECTION, "", 0x01,
                                                  connection_payload(resource_id, **payload_kw)))
        time.sleep(0.2)
        send_ws_frame(sock, 0x2, make_event_frame(0x01, EVENT_START_SESSION, session_id, 0x01,
                                                  session_payload(resource_id, **payload_kw)))
        time.sleep(0.3)

        # 按真实时长实时节奏分包发送（200ms 一档）。
        chunk_size = 2000
        n_chunks = max(1, (len(ogg) + chunk_size - 1) // chunk_size)
        pace_s = (duration_s / n_chunks) if duration_s > 0 else 0.2

        t_start = time.monotonic()
        t_audio_end = t_start
        offset = 0
        while offset < len(ogg):
            chunk = ogg[offset:offset + chunk_size]
            send_ws_frame(sock, 0x2, make_event_frame(0x02, EVENT_TASK_REQUEST, session_id,
                                                      0x00, chunk))
            offset += len(chunk)
            t_audio_end = time.monotonic()
            time.sleep(pace_s)

        send_ws_frame(sock, 0x2, make_event_frame(0x01, EVENT_FINISH_SESSION, session_id, 0x01,
                                                  connection_payload(resource_id, **payload_kw)))

        deadline = time.monotonic() + timeout
        session_finished = False
        while time.monotonic() < deadline and not session_finished:
            sock.settimeout(max(0.1, deadline - time.monotonic()))
            try:
                opcode, payload = recv_ws_frame(sock)
            except socket.timeout:
                break
            if opcode == 0x9:  # ping
                send_ws_frame(sock, 0xA, payload)
                continue
            if opcode not in {0x1, 0x2}:
                continue
            parsed_evt = parse_event_frame(payload)
            if parsed_evt is None:
                continue
            event_id, text = parsed_evt
            if event_id == -1:
                res.server_errors.append(text)
                continue
            now = time.monotonic()
            if event_id in {451, 152} and text:
                try:
                    result = json.loads(text).get("result", {})
                    asr_text = result.get("text", "")
                    utterances = result.get("utterances", [])
                    definite = any(u.get("definite") for u in utterances)
                except (json.JSONDecodeError, AttributeError):
                    asr_text, definite = text, False
                if event_id == 152 and asr_text:
                    definite = True
                if asr_text:
                    if res.first_partial_latency_ms is None:
                        res.first_partial_latency_ms = (now - t_start) * 1000.0
                    res.partial_count += 1
                    if definite:
                        finals.append(asr_text)
                    else:
                        partials.append(asr_text)
            if event_id == 152:
                session_finished = True
                t_final = now
                res.tail_latency_ms = (t_final - t_audio_end) * 1000.0
                res.total_latency_ms = (t_final - t_start) * 1000.0

        try:
            send_ws_frame(sock, 0x2, make_event_frame(0x01, EVENT_FINISH_CONNECTION, "", 0x01,
                                                      connection_payload(resource_id, **payload_kw)))
        except OSError:
            pass

        res.final_text = finals[-1] if finals else (partials[-1] if partials else "")
        if not session_finished:
            res.error = "timeout: no session_finished"
        elif not finals:
            res.error = "no definite result"
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
