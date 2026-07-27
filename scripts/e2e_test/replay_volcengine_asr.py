#!/usr/bin/env python3
"""Replay a DebugAudio ogg against real Volcengine ASR with configurable hotwords.

用途：热词/语料问题的对照实验。同一段录音（来自 %APPDATA%\\VoiceStick\\*.ogg
调试缓存）在不同 hotwords 配置下重放，观察火山识别结果差异，不伪造结果。

协议与 desktop/windows/src/asr_protocol.cc 保持一致：可复用连接事件帧
（start_connection/start_session/task_request/finish_session），JSON 不压缩，
音频帧 serialization=0、compression=0。

示例：
    python scripts/e2e_test/replay_volcengine_asr.py \
        "%APPDATA%\\VoiceStick\\20260728-052224-VS-53A8-session-7.ogg" \
        --hotwords "AGENTS.md,CLAUDE.md,VoiceStick"
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import secrets
import socket
import ssl
import struct
import sys
import time
from pathlib import Path
from urllib.parse import urlparse

VOLCENGINE_URL = "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async"
DEFAULT_RESOURCE_ID = "volc.bigasr.sauc.duration"

EVENT_START_CONNECTION = 1
EVENT_FINISH_CONNECTION = 2
EVENT_START_SESSION = 100
EVENT_FINISH_SESSION = 102
EVENT_TASK_REQUEST = 200

EVENT_NAMES = {
    50: "connection_started",
    51: "connection_failed",
    52: "connection_finished",
    150: "session_started",
    151: "session_canceled",
    152: "session_finished",
    154: "usage",
    450: "asr_info",
    451: "asr_response",
    459: "asr_end",
}


def load_windows_config() -> dict[str, str]:
    appdata = os.environ.get("APPDATA")
    if not appdata:
        return {}
    path = Path(appdata) / "VoiceStick" / "config.toml"
    values: dict[str, str] = {}
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError:
        return values
    for raw in lines:
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        value = value.strip()
        if len(value) >= 2 and value[0] == '"' and value[-1] == '"':
            value = value[1:-1].replace('\\"', '"').replace("\\\\", "\\")
        values[key.strip()] = value
    return values


def estimate_tokens(word: str) -> int:
    """与 AsrProtocol::EstimateHotwordTokens 相同的粗略估算。"""
    cjk = sum(1 for ch in word if ord(ch) >= 0x2E80)
    ascii_chars = len(word) - cjk
    tokens = cjk + (ascii_chars + 2) // 3
    return max(tokens, 1) if word else 0


def session_payload(resource_id: str, hotwords: list[str],
                    enable_nonstream: bool = True, enable_ddc: bool = True,
                    dialog_context: str = "",
                    boosting_table_id: str = "", correct_table_id: str = "") -> bytes:
    request = {
        "model_name": "bigmodel",
        "enable_nonstream": enable_nonstream,
        "show_utterances": False,
        "result_type": "full",
        "enable_ddc": enable_ddc,
        "resource_id": resource_id,
    }
    corpus: dict[str, str] = {}
    if boosting_table_id:
        corpus["boosting_table_id"] = boosting_table_id
    if correct_table_id:
        corpus["correct_table_id"] = correct_table_id
    if dialog_context:
        corpus["context"] = json.dumps({"context_type": "dialog_ctx",
                                        "context_data": [{"text": dialog_context}]},
                                       ensure_ascii=False, separators=(",", ":"))
    elif hotwords:
        corpus["context"] = json.dumps({"hotwords": [{"word": w} for w in hotwords]},
                                       ensure_ascii=False, separators=(",", ":"))
    if corpus:
        request["corpus"] = corpus
    payload = {
        "user": {"uid": "voice-stick-local"},
        "audio": {"format": "ogg", "codec": "opus", "rate": 16000, "bits": 16, "channel": 1},
        "request": request,
    }
    return json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def connection_payload(resource_id: str) -> bytes:
    inner = session_payload(resource_id, [])
    body = b'{"namespace":"BidirectionalASR","event":0,"req_params":' + inner + b"}"
    return body


def make_event_frame(message_type: int, event: int, session_id: str,
                     serialization: int, payload: bytes) -> bytes:
    out = bytearray([0x11, (message_type << 4) | 0x04, serialization << 4, 0x00])
    out += struct.pack(">I", event)
    if session_id:
        session_bytes = session_id.encode("utf-8")
        out += struct.pack(">I", len(session_bytes)) + session_bytes
    out += struct.pack(">I", len(payload)) + payload
    return bytes(out)


def make_legacy_frame(message_type: int, flags: int, serialization: int, payload: bytes) -> bytes:
    """传统（非可复用）协议帧：full client request / audio only request。"""
    return bytes([0x11, (message_type << 4) | flags, serialization << 4, 0x00]) + \
        struct.pack(">I", len(payload)) + payload


def parse_legacy_response(payload: bytes) -> tuple[bool, str] | None:
    """解析传统协议 full server response，返回 (is_last, text_json)。"""
    if len(payload) < 4:
        return None
    message_type = payload[1] >> 4
    flags = payload[1] & 0x0F
    compression = payload[2] & 0x0F
    if message_type == 0x0F:
        offset = (payload[0] & 0x0F) * 4
        if len(payload) >= offset + 8:
            code = struct.unpack(">I", payload[offset:offset + 4])[0]
            msg_size = struct.unpack(">I", payload[offset + 4:offset + 8])[0]
            msg = payload[offset + 8:offset + 8 + msg_size].decode("utf-8", "replace")
            return (True, f"error {code}: {msg}")
        return None
    if message_type != 0x09 or compression != 0x00:
        return None
    offset = (payload[0] & 0x0F) * 4
    if flags in {0x01, 0x03}:
        offset += 4  # sequence
    if len(payload) < offset + 4:
        return None
    payload_size = struct.unpack(">I", payload[offset:offset + 4])[0]
    offset += 4
    text = payload[offset:offset + payload_size].decode("utf-8", "replace")
    return (flags == 0x03, text)


def read_exact(sock: socket.socket, size: int) -> bytes:
    chunks = bytearray()
    while len(chunks) < size:
        chunk = sock.recv(size - len(chunks))
        if not chunk:
            raise EOFError("socket closed")
        chunks += chunk
    return bytes(chunks)


def send_ws_frame(sock: socket.socket, opcode: int, payload: bytes, fin: bool = True) -> None:
    first = (0x80 if fin else 0) | opcode
    length = len(payload)
    header = bytearray([first])
    if length < 126:
        header.append(0x80 | length)
    elif length <= 0xFFFF:
        header.append(0x80 | 126)
        header += struct.pack(">H", length)
    else:
        header.append(0x80 | 127)
        header += struct.pack(">Q", length)
    mask = secrets.token_bytes(4)
    masked = bytes(byte ^ mask[i % 4] for i, byte in enumerate(payload))
    sock.sendall(bytes(header) + mask + masked)


def recv_ws_frame(sock: socket.socket) -> tuple[int, bytes]:
    first, second = read_exact(sock, 2)
    opcode = first & 0x0F
    masked = bool(second & 0x80)
    length = second & 0x7F
    if length == 126:
        length = struct.unpack(">H", read_exact(sock, 2))[0]
    elif length == 127:
        length = struct.unpack(">Q", read_exact(sock, 8))[0]
    mask = read_exact(sock, 4) if masked else b""
    payload = read_exact(sock, length)
    if masked:
        payload = bytes(byte ^ mask[i % 4] for i, byte in enumerate(payload))
    return opcode, payload


def websocket_handshake(url: str, api_key: str, resource_id: str, timeout: float) -> socket.socket:
    parsed = urlparse(url)
    host = parsed.hostname
    if not host:
        raise ValueError("URL has no host")
    port = parsed.port or (443 if parsed.scheme == "wss" else 80)
    path = parsed.path or "/"
    raw = socket.create_connection((host, port), timeout=timeout)
    raw.settimeout(timeout)
    sock = ssl.create_default_context().wrap_socket(raw, server_hostname=host) \
        if parsed.scheme == "wss" else raw

    key = base64.b64encode(secrets.token_bytes(16)).decode("ascii")
    headers = [
        f"GET {path} HTTP/1.1",
        f"Host: {host}:{port}" if parsed.port else f"Host: {host}",
        "Upgrade: websocket",
        "Connection: Upgrade",
        f"Sec-WebSocket-Key: {key}",
        "Sec-WebSocket-Version: 13",
        "User-Agent: VoiceStick-ASR-Replay/1.0",
        f"X-Api-Key: {api_key}",
        f"X-Api-Resource-Id: {resource_id}",
        f"X-Api-Request-Id: voice-stick-replay-{secrets.token_hex(8)}",
        "X-Api-Sequence: -1",
        "",
        "",
    ]
    sock.sendall("\r\n".join(headers).encode("ascii"))
    response = bytearray()
    while b"\r\n\r\n" not in response:
        response += sock.recv(4096)
        if len(response) > 65536:
            raise RuntimeError("oversized handshake response")
    status = response.split(b"\r\n", 1)[0].decode("iso-8859-1", errors="replace")
    if " 101 " not in status:
        raise RuntimeError(f"websocket upgrade failed: {status}")
    return sock


def parse_event_frame(payload: bytes) -> tuple[int, str] | None:
    """解析服务端事件帧，返回 (event_id, payload_text)。"""
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


def main() -> int:
    config = load_windows_config()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ogg", help="DebugAudio ogg 文件路径")
    parser.add_argument("--hotwords", default="",
                        help="逗号分隔热词；空串=不带 corpus；默认读 config.toml 的 asr_hotwords")
    parser.add_argument("--no-config-hotwords", action="store_true",
                        help="忽略 config.toml 的 asr_hotwords（默认 --hotwords 为空时读配置）")
    parser.add_argument("--api-key", default=os.environ.get("VOICESTICK_ASR_API_KEY")
                        or config.get("volcengine_api_key"))
    parser.add_argument("--resource-id", default=os.environ.get("VOICESTICK_ASR_RESOURCE_ID")
                        or config.get("resource_id") or DEFAULT_RESOURCE_ID)
    parser.add_argument("--url", default=os.environ.get("VOICESTICK_ASR_URL") or VOLCENGINE_URL)
    parser.add_argument("--chunk-ms", type=int, default=200, help="音频分包间隔 ms")
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--quiet", action="store_true", help="只打印最终识别文本")
    parser.add_argument("--no-nonstream", action="store_true", help="关闭 enable_nonstream 二遍识别")
    parser.add_argument("--no-ddc", action="store_true", help="关闭 enable_ddc 语义顺滑")
    parser.add_argument("--dialog-context", default="",
                        help="用 dialog_ctx 上下文形式传 corpus（与 hotwords 互斥，优先）")
    parser.add_argument("--boosting-table-id", default="", help="自学习平台热词表 ID")
    parser.add_argument("--correct-table-id", default="", help="自学习平台替换词表 ID")
    parser.add_argument("--legacy", action="store_true",
                        help="用传统 full-client-request 协议（bigmodel_nostream 端点需要）")
    args = parser.parse_args()

    if not args.api_key:
        print("缺少 volcengine_api_key（config.toml 或 VOICESTICK_ASR_API_KEY）", file=sys.stderr)
        return 2

    ogg = Path(args.ogg).read_bytes()
    if not ogg:
        print(f"ogg 文件为空: {args.ogg}", file=sys.stderr)
        return 2

    if args.hotwords:
        hotwords = [w.strip() for w in args.hotwords.split(",") if w.strip()]
    elif not args.no_config_hotwords:
        hotwords = [w.strip() for w in config.get("asr_hotwords", "").split(",") if w.strip()]
    else:
        hotwords = []

    est_total = sum(estimate_tokens(w) for w in hotwords)
    if not args.quiet:
        print(f"ogg={args.ogg} ({len(ogg)} bytes) hotwords={len(hotwords)} 个, "
              f"估算 {est_total} tokens（客户端预算 80）")
        if hotwords:
            print(f"  corpus.context={json.dumps({'hotwords': [{'word': w} for w in hotwords]}, ensure_ascii=False)}")

    sock = websocket_handshake(args.url, args.api_key, args.resource_id, args.timeout)
    session_id = secrets.token_hex(8)
    finals: list[str] = []
    partials: list[str] = []

    if args.legacy:
        # 传统协议：full client request -> audio only requests -> 负包 -> full server response。
        try:
            send_ws_frame(sock, 0x2, make_legacy_frame(
                0x01, 0x00, 0x01,
                session_payload(args.resource_id, hotwords, False, not args.no_ddc,
                                args.dialog_context, args.boosting_table_id,
                                args.correct_table_id)))
            offset = 0
            while offset < len(ogg):
                chunk = ogg[offset:offset + 2000]
                offset += len(chunk)
                last = offset >= len(ogg)
                send_ws_frame(sock, 0x2, make_legacy_frame(0x02, 0x02 if last else 0x00,
                                                           0x00, chunk))
                if not last:
                    time.sleep(args.chunk_ms / 1000.0)

            deadline = time.monotonic() + args.timeout
            is_last = False
            while time.monotonic() < deadline and not is_last:
                sock.settimeout(max(0.1, deadline - time.monotonic()))
                try:
                    opcode, payload = recv_ws_frame(sock)
                except socket.timeout:
                    break
                if opcode == 0x9:
                    send_ws_frame(sock, 0xA, payload)
                    continue
                if opcode not in {0x1, 0x2}:
                    continue
                parsed = parse_legacy_response(payload)
                if parsed is None:
                    continue
                is_last, text = parsed
                if text.startswith("error "):
                    print(f"  SERVER ERROR: {text}", file=sys.stderr)
                    return 1
                try:
                    asr_text = json.loads(text).get("result", {}).get("text", "")
                except json.JSONDecodeError:
                    asr_text = text
                if asr_text:
                    finals.append(asr_text)
                    if not args.quiet:
                        print(f"  [{'last' if is_last else 'resp'}] {asr_text}")
        finally:
            sock.close()
        print(f"RESULT: {finals[-1] if finals else ''}")
        return 0 if finals else 1

    try:
        send_ws_frame(sock, 0x2, make_event_frame(0x01, EVENT_START_CONNECTION, "", 0x01,
                                                  connection_payload(args.resource_id)))
        time.sleep(0.2)
        send_ws_frame(sock, 0x2, make_event_frame(0x01, EVENT_START_SESSION, session_id, 0x01,
                                                  session_payload(args.resource_id, hotwords,
                                                                  not args.no_nonstream,
                                                                  not args.no_ddc,
                                                                  args.dialog_context,
                                                                  args.boosting_table_id,
                                                                  args.correct_table_id)))
        time.sleep(0.3)

        # 分包发送音频，模拟实时节奏。
        chunk_size = max(1, int(len(ogg) / max(1, (len(ogg) // 2000) or 1)))
        offset = 0
        while offset < len(ogg):
            chunk = ogg[offset:offset + 2000]
            send_ws_frame(sock, 0x2, make_event_frame(0x02, EVENT_TASK_REQUEST, session_id,
                                                      0x00, chunk))
            offset += len(chunk)
            time.sleep(args.chunk_ms / 1000.0)

        send_ws_frame(sock, 0x2, make_event_frame(0x01, EVENT_FINISH_SESSION, session_id, 0x01,
                                                  connection_payload(args.resource_id)))

        deadline = time.monotonic() + args.timeout
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
            name = EVENT_NAMES.get(event_id, f"event_{event_id}")
            if event_id == -1:
                print(f"  SERVER ERROR: {text}", file=sys.stderr)
                return 1
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
                if definite:
                    finals.append(asr_text)
                    if not args.quiet:
                        print(f"  [final] {asr_text}")
                elif asr_text:
                    partials.append(asr_text)
                    if not args.quiet:
                        print(f"  [partial] {asr_text}")
            elif not args.quiet:
                print(f"  [{name}] {text[:200]}")
            if event_id == 152:
                session_finished = True

        send_ws_frame(sock, 0x2, make_event_frame(0x01, EVENT_FINISH_CONNECTION, "", 0x01,
                                                  connection_payload(args.resource_id)))
    finally:
        sock.close()

    final_text = finals[-1] if finals else (partials[-1] if partials else "")
    print(f"RESULT: {final_text}")
    return 0 if finals else 1


if __name__ == "__main__":
    raise SystemExit(main())
